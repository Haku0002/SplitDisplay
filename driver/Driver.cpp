/*++
    SplitDisplay indirect display driver.

    Derived from the Microsoft IddSampleDriver (MIT). Exposes up to sixteen virtual monitors that are
    plugged on demand through the shared-memory contract in protocol.h, and republishes every
    desktop frame into a ring of shared textures consumed by the SplitDisplay compositor.
--*/

#include "Driver.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <numeric>

using namespace Microsoft::WRL;
using namespace SplitDisplay;

#pragma region Globals

static SdShared* g_Shm = nullptr;

// Only one adapter instance per host process may own the shared memory and plug monitors;
// a duplicate root device node would otherwise double the monitors.
static std::atomic<void*> g_Owner = nullptr;

// Config captured when monitors were plugged; the mode callbacks answer from this snapshot.
// Guarded by a lock: callbacks run on OS threads while the command thread replaces it.
static std::mutex g_CfgLock;
static SdConfig g_Cfg = { 2, 0, {}, { { 2560, 1440, 60000, 0, {}, 0, 0 }, { 2560, 1440, 60000, 0, {}, 0, 0 } } };

static SdConfig CfgSnapshot()
{
    std::lock_guard lk(g_CfgLock);
    return g_Cfg;
}

static void SetCfgSnapshot(const SdConfig& cfg)
{
    std::lock_guard lk(g_CfgLock);
    g_Cfg = cfg;
}

// Stable container ID per monitor slot: {9A3C7E41-2B6D-4F18-8E0C-5D7A1B2C3Exx}, xx = slot + 1.
static GUID ContainerId(UINT index)
{
    GUID g = { 0x9a3c7e41, 0x2b6d, 0x4f18, { 0x8e, 0x0c, 0x5d, 0x7a, 0x1b, 0x2c, 0x3e, 0x00 } };
    g.Data4[7] = (BYTE)(index + 1);
    return g;
}

// Pixel pitch used for the EDID's physical size (a 13.3" 2560-wide panel), so Windows'
// scaling recommendation matches the real panel.
static constexpr double kMmPerPixel = 0.1148;

void SplitDisplay::DLog(const wchar_t* fmt, ...)
{
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1200];
    swprintf_s(line, L"%02u:%02u:%02u.%03u [idd %lu] %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);
    OutputDebugStringW(line);

    static std::mutex lock;
    std::lock_guard lk(lock);
    // Same place the user-mode log uses (%ProgramData% may not be on C:), rotated once per
    // driver load so years of plug/unplug cycles cannot grow it without bound.
    wchar_t dir[MAX_PATH] = L"C:\\ProgramData";
    if (!GetEnvironmentVariableW(L"ProgramData", dir, MAX_PATH)) wcscpy_s(dir, L"C:\\ProgramData");
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\SplitDisplay\\idd.log", dir);
    static bool rotated = false;
    if (!rotated)
    {
        rotated = true;
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad) && fad.nFileSizeLow > 5 * 1024 * 1024 && !fad.nFileSizeHigh)
        {
            wchar_t old[MAX_PATH];
            swprintf_s(old, L"%s.old", path);
            MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
        }
    }
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        char utf8[2400];
        int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
        DWORD written;
        if (n > 1) WriteFile(f, utf8, n - 1, &written, nullptr);
        CloseHandle(f);
    }
}

#pragma endregion

#pragma region EDID and modes

// Builds a 128-byte EDID 1.4 block describing one region of the panel, named "Split <index+1>".
static void BuildEdid(UINT index, const SdConfig& cfg, BYTE (&e)[128])
{
    const UINT w = cfg.mon[index].width, h = cfg.mon[index].height, milliHz = cfg.mon[index].refreshMilliHz;
    // Real region size on the physical panel when the compositor knows it; the panel pitch is
    // only a fallback guess. EDID image-size bytes top out at 255 cm, detailed sizes at 4095 mm.
    const UINT hmm = std::min<UINT>(cfg.mon[index].widthMm ? cfg.mon[index].widthMm : (UINT)(w * kMmPerPixel + 0.5), 4095);
    const UINT vmm = std::min<UINT>(cfg.mon[index].heightMm ? cfg.mon[index].heightMm : (UINT)(h * kMmPerPixel + 0.5), 4095);
    ZeroMemory(e, sizeof(e));
    const BYTE header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    memcpy(e, header, 8);

    // Manufacturer "SPD": 5-bit letters, big endian.
    UINT16 mfg = (UINT16)((('S' - '@') << 10) | (('P' - '@') << 5) | ('D' - '@'));
    e[8] = (BYTE)(mfg >> 8);
    e[9] = (BYTE)(mfg & 0xFF);
    e[10] = (BYTE)(index + 1); // product code, also used to identify the monitor in ParseMonitorDescription
    e[11] = 0x00;
    e[12] = (BYTE)(index + 1); // serial
    e[16] = 1;                 // week
    e[17] = 2026 - 1990;       // year
    e[18] = 1;
    e[19] = 4;                 // EDID 1.4
    e[20] = 0xA5;              // digital, 8 bpc, DisplayPort
    e[21] = (BYTE)((hmm + 5) / 10); // cm
    e[22] = (BYTE)((vmm + 5) / 10);
    e[23] = 120;               // gamma 2.2
    e[24] = 0x06;              // sRGB default, preferred timing is native
    const BYTE srgb[10] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };
    memcpy(e + 25, srgb, 10);
    for (int i = 38; i < 54; i++) e[i] = 0x01; // no standard timings

    // Detailed timing descriptor: reduced-blanking style numbers, only the active area really matters.
    UINT hblank = 160, hfront = 48, hsync = 32;
    UINT vblank = 60, vfront = 3, vsync = 5;
    UINT64 pclk10k = ((UINT64)(w + hblank) * (h + vblank) * milliHz / 1000 + 5000) / 10000;
    if (pclk10k > 65535) pclk10k = 65535; // the detailed-timing pixel clock is 16-bit (655.35 MHz)
    BYTE* d = e + 54;
    d[0] = (BYTE)(pclk10k & 0xFF);
    d[1] = (BYTE)(pclk10k >> 8);
    d[2] = (BYTE)(w & 0xFF);
    d[3] = (BYTE)(hblank & 0xFF);
    d[4] = (BYTE)(((w >> 8) << 4) | (hblank >> 8));
    d[5] = (BYTE)(h & 0xFF);
    d[6] = (BYTE)(vblank & 0xFF);
    d[7] = (BYTE)(((h >> 8) << 4) | (vblank >> 8));
    d[8] = (BYTE)(hfront & 0xFF);
    d[9] = (BYTE)(hsync & 0xFF);
    d[10] = (BYTE)(((vfront & 0xF) << 4) | (vsync & 0xF));
    d[11] = (BYTE)(((hfront >> 8) << 6) | ((hsync >> 8) << 4) | ((vfront >> 4) << 2) | (vsync >> 4));
    d[12] = (BYTE)(hmm & 0xFF);
    d[13] = (BYTE)(vmm & 0xFF);
    d[14] = (BYTE)(((hmm >> 8) << 4) | (vmm >> 8));
    d[17] = 0x1E; // digital separate sync, +h +v

    // Monitor name descriptor.
    BYTE* n = e + 72;
    n[3] = 0xFC;
    char name[14];
    sprintf_s(name, "Split %u", index + 1);
    size_t len = strlen(name);
    for (int i = 0; i < 13; i++)
        n[5 + i] = (BYTE)(i < (int)len ? name[i] : (i == (int)len ? 0x0A : 0x20));

    // Range limits descriptor.
    BYTE* r = e + 90;
    r[3] = 0xFD;
    r[5] = 24;   // min V Hz
    r[6] = 240;  // max V Hz
    r[7] = 15;   // min H kHz
    r[8] = 255;  // max H kHz
    UINT64 pclkHz = (UINT64)(w + hblank) * (h + vblank) * milliHz / 1000;
    r[9] = (BYTE)std::clamp<UINT64>((pclkHz + 9999999) / 10000000, 1, 255); // max pixel clock / 10 MHz
    r[10] = 0x00;
    r[11] = 0x0A;
    for (int i = 12; i < 18; i++) r[i] = 0x20;

    // Dummy descriptor.
    e[108 + 3] = 0x10;

    BYTE sum = 0;
    for (int i = 0; i < 127; i++) sum += e[i];
    e[127] = (BYTE)(0x100 - sum);
}

static void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD MilliHz, bool bMonitorMode)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;
    Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;
    // Fractional rates (59.94, 119.88, ...) survive as rationals, so the virtual monitors are
    // paced at exactly the panel rate instead of drifting against it and dropping a frame
    // whenever the phases cross.
    auto rational = [](UINT64 num, UINT64 den) {
        UINT64 g = std::gcd(num, den);
        DISPLAYCONFIG_RATIONAL r = { (UINT32)std::min<UINT64>(num / g, 0xFFFFFFFFull), (UINT32)(den / g) };
        return r;
    };
    Mode.vSyncFreq = rational(MilliHz, 1000);
    Mode.hSyncFreq = rational((UINT64)MilliHz * Height, 1000);
    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    Mode.pixelRate = (UINT64)MilliHz * Width * Height / 1000;
}

struct ModeTriple
{
    DWORD w, h, milliHz;
};

// Modes of monitor slot `index`: its region size at the panel's current rate (preferred, always
// first), plus the other rates the panel supports. Picking another rate in Settings makes
// SplitDisplay switch the whole panel, so all split monitors of a display always share one rate.
// With scaled modes, 3/4 and 1/2 resolutions are offered too (for fullscreen games and legacy
// apps); the compositor stretches those to the region. The EDID parse callback passes
// includeScaled=false because the EDID itself only describes the native timing.
static std::vector<ModeTriple> ModeList(UINT index, bool includeScaled)
{
    if (index >= SD_MAX_MONITORS) index = 0;
    const SdConfig cfg = CfgSnapshot();
    const auto& s = cfg.mon[index];

    std::vector<DWORD> rates{ s.refreshMilliHz };
    for (UINT i = 0; i < s.rateCount && i < SD_MAX_RATES; i++)
    {
        DWORD r = s.ratesMilliHz[i];
        bool dup = false;
        for (DWORD q : rates) dup |= (r > q ? r - q : q - r) < 100;
        if (!dup && r >= 24000 && r <= 500000) rates.push_back(r);
    }
    std::vector<std::pair<DWORD, DWORD>> sizes{ { s.width, s.height } };
    if (includeScaled)
    {
        for (auto [num, den] : { std::pair<DWORD, DWORD>{ 3, 4 }, { 1, 2 } })
        {
            DWORD sw = s.width * num / den & ~1u, sh = s.height * num / den & ~1u;
            if (sw >= 640 && sh >= 360 && (sw != s.width || sh != s.height)) sizes.push_back({ sw, sh });
        }
    }
    std::vector<ModeTriple> m;
    for (auto [w, h] : sizes)
        for (DWORD r : rates) m.push_back({ w, h, r });
    return m;
}

#pragma endregion

#pragma region WDF plumbing

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD SdDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY SdDeviceD0Entry;
EVT_IDD_CX_ADAPTER_INIT_FINISHED SdAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES SdAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION SdParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES SdMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES SdMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN SdMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN SdMonitorUnassignSwapChain;

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;
    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

// The adapter object carries a pointer to the same context, but must not delete it.
struct IndirectAdapterContextWrapper
{
    IndirectDeviceContext* pContext;
};

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext;
    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(IndirectAdapterContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(_In_ HINSTANCE, _In_ UINT, _In_opt_ LPVOID)
{
    return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    WDF_DRIVER_CONFIG_INIT(&Config, SdDeviceAdd);
    DLog(L"DriverEntry");
    return WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS SdDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = SdDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);
    IddConfig.EvtIddCxAdapterInitFinished = SdAdapterInitFinished;
    IddConfig.EvtIddCxParseMonitorDescription = SdParseMonitorDescription;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = SdMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = SdMonitorQueryModes;
    IddConfig.EvtIddCxAdapterCommitModes = SdAdapterCommitModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = SdMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = SdMonitorUnassignSwapChain;

    NTSTATUS Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if (!NT_SUCCESS(Status)) return Status;

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object) {
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if (pContext) pContext->Cleanup();
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status)) return Status;

    Status = IddCxDeviceInitialize(Device);

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);
    DLog(L"DeviceAdd status=0x%08X", Status);
    return Status;
}

_Use_decl_annotations_
NTSTATUS SdDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext->InitAdapter();
    return STATUS_SUCCESS;
}

#pragma endregion

#pragma region Direct3DDevice

HRESULT Direct3DDevice::Init()
{
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr)) return hr;
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) return hr;
    hr = dev.As(&Device);
    if (FAILED(hr)) return hr;
    hr = ctx.As(&DeviceContext);
    if (FAILED(hr)) return hr;
    // These copies are the first hop of every frame; let them jump the GPU queue ahead of
    // whatever the desktop is rendering, like the compositor already does for its hop.
    ComPtr<IDXGIDevice> dxgi;
    if (SUCCEEDED(dev.As(&dxgi))) dxgi->SetGPUThreadPriority(7);
    return S_OK;
}

#pragma endregion

#pragma region FrameRing

FrameRing::FrameRing(UINT MonitorIndex, std::shared_ptr<Direct3DDevice> Device) : m_Index(MonitorIndex), m_Device(std::move(Device))
{
}

FrameRing::~FrameRing()
{
    Release();
}

void FrameRing::Release()
{
    if (g_Shm)
    {
        auto& m = g_Shm->mon[m_Index];
        InterlockedIncrement(&m.handleGeneration); // odd: handles invalid
        InterlockedExchange(&m.latest, -1);
        for (int i = 0; i < SD_BUFFERS; i++)
        {
            m.texHandle[i] = 0;
            m.slotSeq[i] = 0;
            m.presentQpc[i] = 0;
        }
        for (auto& j : m.journal) j.seq = j.seqEnd = 0;
        m.fenceHandle = 0;
        MemoryBarrier();
        InterlockedIncrement(&m.handleGeneration); // even again, but with no buffers
    }
    for (auto& h : m_TexHandle)
    {
        if (h) CloseHandle(h);
        h = nullptr;
    }
    if (m_FenceHandle) CloseHandle(m_FenceHandle);
    m_FenceHandle = nullptr;
    for (auto& t : m_Tex) t.Reset();
    m_Fence.Reset();
    m_Ready = false;
}

HRESULT FrameRing::Recreate(const D3D11_TEXTURE2D_DESC& Src)
{
    Release();
    if (!g_Shm) return E_FAIL;
    auto& m = g_Shm->mon[m_Index];

    InterlockedIncrement(&m.handleGeneration); // odd while we populate

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = Src.Width;
    d.Height = Src.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = Src.Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = S_OK;
    for (int i = 0; i < SD_BUFFERS && SUCCEEDED(hr); i++)
    {
        hr = m_Device->Device->CreateTexture2D(&d, nullptr, &m_Tex[i]);
        if (FAILED(hr)) break;
        ComPtr<IDXGIResource1> res;
        hr = m_Tex[i].As(&res);
        if (FAILED(hr)) break;
        hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &m_TexHandle[i]);
    }
    if (SUCCEEDED(hr)) hr = m_Device->Device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_Fence));
    if (SUCCEEDED(hr)) hr = m_Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &m_FenceHandle);
    if (FAILED(hr))
    {
        DLog(L"mon%u: ring create failed 0x%08X", m_Index, hr);
        InterlockedIncrement(&m.handleGeneration);
        Release();
        return hr;
    }

    m_FenceValue = 0;
    for (int i = 0; i < SD_BUFFERS; i++)
    {
        m_Accum[i].clear();
        m_AccumFull[i] = true; // a fresh texture holds nothing yet
    }
    m_Width = d.Width;
    m_Height = d.Height;
    m_Format = d.Format;
    for (int i = 0; i < SD_BUFFERS; i++)
    {
        m.texHandle[i] = (UINT64)(ULONG_PTR)m_TexHandle[i];
        m.fenceValue[i] = 0;
    }
    m.fenceHandle = (UINT64)(ULONG_PTR)m_FenceHandle;
    m.adapter = m_Device->AdapterLuid;
    m.width = d.Width;
    m.height = d.Height;
    m.format = (UINT32)d.Format;
    InterlockedExchange(&m.latest, -1);
    MemoryBarrier();
    InterlockedIncrement(&m.handleGeneration); // even: published
    m_Ready = true;
    DLog(L"mon%u: ring %ux%u fmt=%u on adapter %08X:%08X gen=%ld", m_Index, d.Width, d.Height, d.Format,
        m_Device->AdapterLuid.HighPart, m_Device->AdapterLuid.LowPart, m.handleGeneration);
    return S_OK;
}

void FrameRing::Publish(ID3D11Texture2D* Source, IDDCX_SWAPCHAIN SwapChain, const IDDCX_METADATA& Meta)
{
    if (!g_Shm) return;
    D3D11_TEXTURE2D_DESC sd;
    Source->GetDesc(&sd);
    if (!m_Ready || sd.Width != m_Width || sd.Height != m_Height || sd.Format != m_Format)
    {
        if (FAILED(Recreate(sd))) return;
    }

    // What changed in this frame? A single all-zero dirty rect means "nothing": the OS
    // re-presents static desktops so encoders can re-encode; copying those would republish
    // the identical frame and burn both copy hops for no reason. Move regions are gone in
    // IddCx 1.7+ (always zero here); a nonzero count on an older OS falls back to a full copy.
    std::vector<RECT> frameDirty;
    bool full = Meta.HwProtectedSurface || Meta.MoveRegionCount != 0 || Meta.DirtyRectCount == 0;
    if (!full && Meta.DirtyRectCount <= 4096)
    {
        // The usual desktop frame has a handful of rects; only pathological ones go to the heap.
        RECT stackRects[64];
        std::vector<RECT> heapRects;
        RECT* rects = stackRects;
        if (Meta.DirtyRectCount > ARRAYSIZE(stackRects))
        {
            heapRects.resize(Meta.DirtyRectCount);
            rects = heapRects.data();
        }
        IDARG_IN_GETDIRTYRECTS in = { Meta.DirtyRectCount, rects };
        IDARG_OUT_GETDIRTYRECTS out = {};
        if (SUCCEEDED(IddCxSwapChainGetDirtyRects(SwapChain, &in, &out)))
        {
            UINT n = std::min(out.DirtyRectOutCount, Meta.DirtyRectCount);
            if (n == 1 && rects[0].left == 0 && rects[0].top == 0 && rects[0].right == 0 && rects[0].bottom == 0) return; // no image update
            for (UINT i = 0; i < n; i++)
            {
                RECT r = { std::clamp<LONG>(rects[i].left, 0, (LONG)sd.Width), std::clamp<LONG>(rects[i].top, 0, (LONG)sd.Height),
                    std::clamp<LONG>(rects[i].right, 0, (LONG)sd.Width), std::clamp<LONG>(rects[i].bottom, 0, (LONG)sd.Height) };
                if (r.right > r.left && r.bottom > r.top) frameDirty.push_back(r);
            }
            if (frameDirty.empty()) full = true; // OS said something changed but listed nothing usable
        }
        else
        {
            full = true;
        }
    }
    else if (Meta.DirtyRectCount > 4096)
    {
        full = true;
    }

    // Every slot still missing this frame's changes accumulates them: slot w is only correct
    // once it has received all changes since the frame it currently holds.
    for (int i = 0; i < SD_BUFFERS; i++)
    {
        if (full) m_AccumFull[i] = true;
        else if (!m_AccumFull[i])
        {
            m_Accum[i].insert(m_Accum[i].end(), frameDirty.begin(), frameDirty.end());
            if (m_Accum[i].size() > 256) m_AccumFull[i] = true; // a full copy beats hundreds of small ones
        }
    }

    auto& m = g_Shm->mon[m_Index];
    int w = SdPickWriteSlot(m.latest, m.readerIndex);
    if (w < 0) return; // shared state out of range (torn down or corrupted): drop the frame

    auto* ctx = m_Device->DeviceContext.Get();
    bool copyFull = m_AccumFull[w];
    if (!copyFull)
    {
        UINT64 area = 0;
        for (auto& r : m_Accum[w]) area += (UINT64)(r.right - r.left) * (r.bottom - r.top);
        copyFull = area >= (UINT64)sd.Width * sd.Height * 3 / 4;
    }
    if (copyFull)
    {
        ctx->CopyResource(m_Tex[w].Get(), Source);
    }
    else
    {
        for (auto& r : m_Accum[w])
        {
            D3D11_BOX box{ (UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1 };
            ctx->CopySubresourceRegion(m_Tex[w].Get(), 0, (UINT)r.left, (UINT)r.top, 0, Source, 0, &box);
        }
    }
    m_Accum[w].clear();
    m_AccumFull[w] = false;
    ctx->Signal(m_Fence.Get(), ++m_FenceValue);
    ctx->Flush();

    // Journal first (what changed in this frame only), published state afterwards: a consumer
    // that sees the new latest must also find the matching journal entry.
    UINT64 seq = ++m_Seq;
    SdDirtyFrame& j = m.journal[seq % SD_DIRTY_JOURNAL];
    j.seqEnd = 0;
    MemoryBarrier();
    if (full)
    {
        j.full = 1;
        j.count = 0;
    }
    else if ((int)frameDirty.size() > SD_MAX_DIRTY)
    {
        // Too many rects to store: their bounding box is a correct superset.
        SdRect bb = { (INT32)sd.Width, (INT32)sd.Height, 0, 0 };
        for (auto& r : frameDirty)
        {
            bb.left = std::min(bb.left, (INT32)r.left);
            bb.top = std::min(bb.top, (INT32)r.top);
            bb.right = std::max(bb.right, (INT32)r.right);
            bb.bottom = std::max(bb.bottom, (INT32)r.bottom);
        }
        j.full = 0;
        j.count = 1;
        j.rects[0] = bb;
    }
    else
    {
        j.full = 0;
        j.count = (UINT32)frameDirty.size();
        for (UINT32 i = 0; i < j.count; i++)
            j.rects[i] = { frameDirty[i].left, frameDirty[i].top, frameDirty[i].right, frameDirty[i].bottom };
    }
    j.seq = seq;
    MemoryBarrier();
    j.seqEnd = seq;

    m.fenceValue[w] = m_FenceValue;
    m.presentQpc[w] = Meta.PresentDisplayQPCTime;
    m.slotSeq[w] = seq;
    MemoryBarrier();
    InterlockedExchange(&m.latest, w);
    InterlockedExchange64(&m.frameSeq, (LONG64)seq);
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(UINT MonitorIndex, IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
    : m_Index(MonitorIndex), m_hSwapChain(hSwapChain), m_Device(std::move(Device)), m_hAvailableBufferEvent(NewFrameEvent)
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    SetEvent(m_hTerminateEvent.Get());
    if (m_hThread.Get()) WaitForSingleObject(m_hThread.Get(), INFINITE);
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);
    RunCore();
    // Deleting the swap-chain kicks the OS into providing a new one if needed.
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    m_hSwapChain = nullptr;
    AvRevertMmThreadCharacteristics(AvTaskHandle);
}

void SwapChainProcessor::RunCore()
{
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr)) return;

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();
    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if (FAILED(hr))
    {
        DLog(L"mon%u: SwapChainSetDevice failed 0x%08X", m_Index, hr);
        return;
    }

    FrameRing ring(m_Index, m_Device);
    UINT64 frames = 0;

    for (;;)
    {
        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        if (hr == E_PENDING)
        {
            HANDLE WaitHandles[] = { m_hAvailableBufferEvent, m_hTerminateEvent.Get() };
            // The terminate event is in the wait set, so the timeout is only a safety net for a
            // missed surface event; 100 ms costs nothing next to the old 16 ms idle wakeups.
            DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 100);
            if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT) continue;
            break;
        }
        else if (SUCCEEDED(hr))
        {
            ComPtr<IDXGIResource> AcquiredBuffer;
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(AcquiredBuffer.As(&tex)))
            {
                ring.Publish(tex.Get(), m_hSwapChain, Buffer.MetaData);
                if (++frames == 1) DLog(L"mon%u: first frame", m_Index);
            }
            AcquiredBuffer.Reset();

            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr)) break;
        }
        else
        {
            DLog(L"mon%u: swapchain lost 0x%08X after %llu frames", m_Index, hr, frames);
            break;
        }
    }
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) : m_WdfDevice(WdfDevice)
{
}

IndirectDeviceContext::~IndirectDeviceContext()
{
    if (m_StopEvent) SetEvent(m_StopEvent);
    if (m_Thread)
    {
        WaitForSingleObject(m_Thread, 5000);
        CloseHandle(m_Thread);
    }
    if (m_StopEvent) CloseHandle(m_StopEvent);
    if (m_CmdEvent) CloseHandle(m_CmdEvent);
    // Only the instance that claimed the shared state may tear it down: a duplicate device
    // node coming and going must not pull the mapping out from under the owning instance.
    if (m_Owner)
    {
        if (g_Shm)
        {
            UnmapViewOfFile(g_Shm);
            g_Shm = nullptr;
        }
        if (m_Section) CloseHandle(m_Section);
        ReleaseOwnership();
    }
}

void IndirectDeviceContext::ReleaseOwnership()
{
    void* self = this;
    g_Owner.compare_exchange_strong(self, nullptr);
    m_Owner = false;
}

void IndirectDeviceContext::InitAdapter()
{
    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);
    AdapterCaps.MaxMonitorsSupported = SD_MAX_MONITORS;
    // Zero on purpose: the OS must not re-present a static desktop to us (Publish skips
    // no-update frames anyway, but there is no reason to be woken for them at all).
    AdapterCaps.StaticDesktopReencodeFrameCount = 0;
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"SplitDisplay";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"SplitDisplay";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"SplitDisplay Virtual Panel";

    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectAdapterContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);
    DLog(L"AdapterInitAsync status=0x%08X", Status);
    if (NT_SUCCESS(Status))
    {
        m_Adapter = AdapterInitOut.AdapterObject;
        WdfObjectGet_IndirectAdapterContextWrapper(AdapterInitOut.AdapterObject)->pContext = this;
    }
}

void IndirectDeviceContext::FinishInit()
{
    if (m_Thread) return;

    void* expected = nullptr;
    if (!g_Owner.compare_exchange_strong(expected, this))
    {
        DLog(L"another SplitDisplay adapter already owns the shared memory; this one stays idle");
        return;
    }
    m_Owner = true;
    // From here on, every failure path must ReleaseOwnership(): a leaked claim would leave
    // the driver permanently idle until the host process restarts.
    auto fail = [this] {
        if (m_CmdEvent) CloseHandle(m_CmdEvent);
        if (m_Section) CloseHandle(m_Section);
        m_CmdEvent = m_Section = nullptr;
        ReleaseOwnership();
    };

    // SYSTEM, Administrators, LocalService. LocalService is the UMDF host account: after a
    // host restart it must be able to re-open the section a still-running compositor kept alive.
    SECURITY_ATTRIBUTES sa = { sizeof(sa) };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr))
    {
        DLog(L"SDDL failed %lu", GetLastError());
        fail();
        return;
    }
    m_Section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SdShared), SD_SHM_NAME);
    DWORD sectionErr = GetLastError();
    m_CmdEvent = CreateEventW(&sa, FALSE, FALSE, SD_CMD_EVENT_NAME);
    LocalFree(sa.lpSecurityDescriptor);
    if (!m_Section || !m_CmdEvent)
    {
        DLog(L"shared objects failed: section=%p event=%p err=%lu", m_Section, m_CmdEvent, GetLastError());
        fail();
        return;
    }

    g_Shm = (SdShared*)MapViewOfFile(m_Section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SdShared));
    if (!g_Shm)
    {
        fail();
        return;
    }
    if (sectionErr != ERROR_ALREADY_EXISTS || g_Shm->magic != SD_MAGIC || g_Shm->version != SD_VERSION)
    {
        ZeroMemory(g_Shm, sizeof(SdShared));
        SdWriteConfig(g_Shm, CfgSnapshot());
    }
    else
    {
        SdConfig kept;
        if (SdReadConfig(g_Shm, kept)) SetCfgSnapshot(kept); // a surviving section keeps the plug config
    }
    g_Shm->magic = SD_MAGIC;
    g_Shm->version = SD_VERSION;
    g_Shm->driverPid = GetCurrentProcessId();
    g_Shm->driverEpoch = (UINT32)(GetTickCount64() ^ ((ULONGLONG)GetCurrentProcessId() << 8) ^ 0x5D17u) | 1;
    m_LastModesGeneration = g_Shm->modesGeneration;
    for (auto& m : g_Shm->mon)
    {
        m.plugged = 0;
        m.latest = -1;
        m.readerIndex = -1;
        m.handleGeneration = 0;
    }

    m_StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_Thread = CreateThread(nullptr, 0, CommandThread, this, 0, nullptr);
    DLog(L"shared memory ready, pid=%lu", g_Shm->driverPid);
}

DWORD CALLBACK IndirectDeviceContext::CommandThread(LPVOID Argument)
{
    reinterpret_cast<IndirectDeviceContext*>(Argument)->CommandLoop();
    return 0;
}

void IndirectDeviceContext::CommandLoop()
{
    HANDLE waits[] = { m_StopEvent, m_CmdEvent };
    for (;;)
    {
        InterlockedExchange64(&g_Shm->driverHeartbeat, (LONG64)GetTickCount64());
        bool want = g_Shm->desiredPlugged != 0;
        if (want)
        {
            SdConfig cfg;
            if (SdReadConfig(g_Shm, cfg))
            {
                if (!m_Plugged) ReconcileMonitors(cfg);
                else if (g_Shm->modesGeneration != m_LastModesGeneration)
                {
                    // Same monitors, new mode lists (e.g. the panel switched refresh rate).
                    m_LastModesGeneration = g_Shm->modesGeneration;
                    ReconcileMonitors(cfg); // replugs only slots whose size actually changed
                    UpdateModes(cfg);
                }
            }
        }
        else if (m_Plugged) UnplugMonitors();

        DWORD r = WaitForMultipleObjects(2, waits, FALSE, 1000);
        if (r == WAIT_OBJECT_0) break;
    }
    if (m_Plugged) UnplugMonitors();
}

void IndirectDeviceContext::UpdateModes(const SdConfig& cfg)
{
    SetCfgSnapshot(cfg);
    for (UINT i = 0; i < SD_MAX_MONITORS; i++)
    {
        if (!m_Monitors[i]) continue;
        // The mode callbacks answer from this snapshot; the OS re-reads the target modes
        // from the buffer below. Current slot sizes stay as plugged (reconcile handles those).
        m_Current[i].refreshMilliHz = cfg.mon[i].refreshMilliHz;
        auto modes = ModeList(i, true);
        std::vector<IDDCX_TARGET_MODE> targetModes(modes.size());
        for (size_t k = 0; k < modes.size(); k++)
        {
            targetModes[k] = {};
            targetModes[k].Size = sizeof(IDDCX_TARGET_MODE);
            FillSignalInfo(targetModes[k].TargetVideoSignalInfo.targetVideoSignalInfo, modes[k].w, modes[k].h, modes[k].milliHz, false);
        }
        IDARG_IN_UPDATEMODES in = { IDDCX_UPDATE_REASON_OTHER, (UINT)targetModes.size(), targetModes.data() };
        NTSTATUS Status = IddCxMonitorUpdateModes(m_Monitors[i], &in);
        DLog(L"UpdateModes(%u) -> 0x%08X", i, Status);
    }
}

void IndirectDeviceContext::ReconcileMonitors(const SdConfig& cfg)
{
    bool valid = cfg.count >= 1 && cfg.count <= SD_MAX_MONITORS;
    for (UINT i = 0; valid && i < cfg.count; i++)
    {
        const auto& m = cfg.mon[i];
        valid = m.width >= 160 && m.height >= 160 && m.width <= 16384 && m.height <= 16384 && m.refreshMilliHz >= 24000 && m.refreshMilliHz <= 500000;
    }
    if (!valid)
    {
        DLog(L"rejecting bad config: %u monitors", cfg.count);
        InterlockedExchange(&g_Shm->desiredPlugged, 0);
        UnplugMonitors();
        return;
    }
    SetCfgSnapshot(cfg);

    if ((cfg.renderAdapter.LowPart || cfg.renderAdapter.HighPart) &&
        (cfg.renderAdapter.LowPart != m_RenderAdapter.LowPart || cfg.renderAdapter.HighPart != m_RenderAdapter.HighPart))
    {
        IDARG_IN_ADAPTERSETRENDERADAPTER ra = {};
        ra.PreferredRenderAdapter = cfg.renderAdapter;
        IddCxAdapterSetRenderAdapter(m_Adapter, &ra);
        m_RenderAdapter = cfg.renderAdapter;
        DLog(L"preferred render adapter %08X:%08X", cfg.renderAdapter.HighPart, cfg.renderAdapter.LowPart);
    }

    // Only slots whose size changed (or appeared/disappeared) are re-plugged; a slot that
    // merely changed refresh rate keeps its monitor and is handled by UpdateModes.
    for (UINT i = 0; i < SD_MAX_MONITORS; i++)
    {
        bool want = i < cfg.count;
        bool have = m_Monitors[i] != nullptr;
        bool resized = have && want && (m_Current[i].width != cfg.mon[i].width || m_Current[i].height != cfg.mon[i].height);
        if (have && (!want || resized)) DepartSlot(i);
        if (want && !m_Monitors[i]) CreateSlot(i, cfg);
    }
    m_Plugged = false;
    for (auto* mon : m_Monitors) m_Plugged |= mon != nullptr;
}

bool IndirectDeviceContext::CreateSlot(UINT i, const SdConfig& cfg)
{
    BYTE edid[128];
    BuildEdid(i, cfg, edid);

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object) {
        auto* p = WdfObjectGet_IndirectMonitorContextWrapper(Object);
        if (p) p->Cleanup();
    };

    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    MonitorInfo.ConnectorIndex = i;
    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    MonitorInfo.MonitorDescription.DataSize = sizeof(edid);
    MonitorInfo.MonitorDescription.pData = edid;
    MonitorInfo.MonitorContainerId = ContainerId(i);

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    IDARG_OUT_MONITORCREATE MonitorCreateOut;
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    if (!NT_SUCCESS(Status))
    {
        DLog(L"MonitorCreate(%u) failed 0x%08X", i, Status);
        return false;
    }
    WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject)->pContext = new IndirectMonitorContext(MonitorCreateOut.MonitorObject, i);

    IDARG_OUT_MONITORARRIVAL ArrivalOut;
    Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
    DLog(L"MonitorArrival(%u) %ux%u@%u.%03u status=0x%08X", i, cfg.mon[i].width, cfg.mon[i].height, cfg.mon[i].refreshMilliHz / 1000,
        cfg.mon[i].refreshMilliHz % 1000, Status);
    if (!NT_SUCCESS(Status))
    {
        WdfObjectDelete(MonitorCreateOut.MonitorObject);
        return false;
    }
    m_Monitors[i] = MonitorCreateOut.MonitorObject;
    m_Current[i] = cfg.mon[i];
    InterlockedExchange(&g_Shm->mon[i].plugged, 1);
    return true;
}

void IndirectDeviceContext::DepartSlot(UINT i)
{
    if (!m_Monitors[i]) return;
    IDDCX_MONITOR mon = m_Monitors[i];
    m_Monitors[i] = nullptr;
    NTSTATUS Status = IddCxMonitorDeparture(mon);
    DLog(L"MonitorDeparture(%u) status=0x%08X", i, Status);
    InterlockedExchange(&g_Shm->mon[i].plugged, 0);
    // Destroying the object runs the context cleanup (stops the frame thread, frees the
    // ring). The old code only departed, leaking one monitor object and context per replug.
    WdfObjectDelete((WDFOBJECT)mon);
}

void IndirectDeviceContext::UnplugMonitors()
{
    for (UINT i = 0; i < SD_MAX_MONITORS; i++) DepartSlot(i);
    m_Plugged = false;
}

#pragma endregion

#pragma region IndirectMonitorContext

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor, UINT Index) : m_Monitor(Monitor), m_Index(Index)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    m_ProcessingThread.reset();
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    m_ProcessingThread.reset();
    auto Device = std::make_shared<Direct3DDevice>(RenderAdapter);
    HRESULT hr = Device->Init();
    DLog(L"mon%u: AssignSwapChain render adapter %08X:%08X hr=0x%08X", m_Index, RenderAdapter.HighPart, RenderAdapter.LowPart, hr);
    if (FAILED(hr))
    {
        WdfObjectDelete(SwapChain);
        return;
    }
    m_ProcessingThread.reset(new SwapChainProcessor(m_Index, SwapChain, Device, NewFrameEvent));
}

void IndirectMonitorContext::UnassignSwapChain()
{
    DLog(L"mon%u: UnassignSwapChain", m_Index);
    m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI callbacks

_Use_decl_annotations_
NTSTATUS SdAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    DLog(L"AdapterInitFinished status=0x%08X", pInArgs->AdapterInitStatus);
    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
        WdfObjectGet_IndirectAdapterContextWrapper(AdapterObject)->pContext->FinishInit();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SdAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SdParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    UINT index = 0;
    if (pInArgs->MonitorDescription.DataSize >= 11 && pInArgs->MonitorDescription.pData)
        index = ((const BYTE*)pInArgs->MonitorDescription.pData)[10] - 1u; // product code = slot + 1
    auto modes = ModeList(index, false);
    pOutArgs->MonitorModeBufferOutputCount = (UINT)modes.size();
    if (pInArgs->MonitorModeBufferInputCount < modes.size())
        return pInArgs->MonitorModeBufferInputCount > 0 ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

    if (pInArgs->MonitorDescription.DataSize != 128) return STATUS_INVALID_PARAMETER;

    for (size_t i = 0; i < modes.size(); i++)
    {
        IDDCX_MONITOR_MODE& m = pInArgs->pMonitorModes[i];
        m = {};
        m.Size = sizeof(m);
        m.Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
        FillSignalInfo(m.MonitorVideoSignalInfo, modes[i].w, modes[i].h, modes[i].milliHz, true);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SdMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    auto modes = ModeList(WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject)->pContext->Index(), true);
    pOutArgs->DefaultMonitorModeBufferOutputCount = (UINT)modes.size();
    if (pInArgs->DefaultMonitorModeBufferInputCount == 0) return STATUS_SUCCESS;
    if (pInArgs->DefaultMonitorModeBufferInputCount < modes.size()) return STATUS_BUFFER_TOO_SMALL;
    for (size_t i = 0; i < modes.size(); i++)
    {
        IDDCX_MONITOR_MODE& m = pInArgs->pDefaultMonitorModes[i];
        m = {};
        m.Size = sizeof(m);
        m.Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
        FillSignalInfo(m.MonitorVideoSignalInfo, modes[i].w, modes[i].h, modes[i].milliHz, true);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SdMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    auto modes = ModeList(WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject)->pContext->Index(), true);
    pOutArgs->TargetModeBufferOutputCount = (UINT)modes.size();
    if (pInArgs->TargetModeBufferInputCount >= modes.size())
    {
        for (size_t i = 0; i < modes.size(); i++)
        {
            IDDCX_TARGET_MODE& t = pInArgs->pTargetModes[i];
            t = {};
            t.Size = sizeof(t);
            FillSignalInfo(t.TargetVideoSignalInfo.targetVideoSignalInfo, modes[i].w, modes[i].h, modes[i].milliHz, false);
        }
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SdMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject)->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SdMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject)->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

#pragma endregion
