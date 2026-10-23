#include "frames.h"

#include <d3dcompiler.h>

#include <algorithm>

#include "log.h"
#include "ring.h"

#pragma region FrameSource

void FrameSource::Drop()
{
    for (auto& t : tex) t = nullptr;
    for (auto& v : srv) v = nullptr;
    fence = nullptr;
    attachedKey = -1;
    for (auto& d : drawnSeq) d = -1; // whatever the back buffers hold, it did not come from this ring
}

bool FrameSource::Refresh(ID3D11Device5* dev, SdShared* s, HANDLE driverProc)
{
    auto& m = s->mon[index];
    LONG g1 = m.handleGeneration;
    UINT32 epoch = s->driverEpoch;
    Drop();
    if ((g1 & 1) || m.fenceHandle == 0) return false;
    MemoryBarrier();

    auto open = [&](UINT64 remote, auto&& fn) -> bool {
        HANDLE h = nullptr;
        if (!DuplicateHandle(driverProc, (HANDLE)(ULONG_PTR)remote, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
        HRESULT hr = fn(h);
        CloseHandle(h);
        return SUCCEEDED(hr);
    };
    bool ok = true;
    for (int i = 0; i < SD_BUFFERS && ok; i++)
    {
        ok = open(m.texHandle[i], [&](HANDLE h) { return dev->OpenSharedResource1(h, IID_PPV_ARGS(tex[i].put())); });
        if (ok) dev->CreateShaderResourceView(tex[i].get(), nullptr, srv[i].put()); // only for stretched modes; failure is not fatal
    }
    if (ok) ok = open(m.fenceHandle, [&](HANDLE h) { return dev->OpenSharedFence(h, IID_PPV_ARGS(fence.put())); });

    MemoryBarrier();
    if (!ok || m.handleGeneration != g1 || s->driverEpoch != epoch)
    {
        Drop();
        return false;
    }
    width = m.width;
    height = m.height;
    attachedKey = ((long long)epoch << 32) | (unsigned int)g1;
    Log(L"mon%d: attached ring epoch=%08X gen=%ld %ux%u fmt=%u", index, epoch, g1, width, height, m.format);
    return true;
}

int FrameSource::Acquire(SdShared* s, UINT64& fenceValue)
{
    auto& m = s->mon[index];
    for (int attempt = 0; attempt < 3; attempt++)
    {
        LONG l = m.latest;
        if (l < 0 || l >= SD_BUFFERS) return -1;
        InterlockedExchange(&m.readerIndex, l);
        if (m.latest == l)
        {
            fenceValue = m.fenceValue[l];
            reserved = true;
            return (int)l;
        }
        InterlockedExchange(&m.readerIndex, -1);
    }
    return -1;
}

void FrameSource::Release(SdShared* s)
{
    if (!reserved) return;
    InterlockedExchange(&s->mon[index].readerIndex, -1);
    reserved = false;
}

bool FramesChanged(SdShared* s, const std::vector<FrameSource>& src)
{
    for (auto& f : src)
        if (f.seenKey != FrameRingKey(s, f.index) || SdReadI64(&s->mon[f.index].frameSeq) != f.lastSeenSeq) return true;
    return false;
}

#pragma endregion

#pragma region StretchBlitter

bool StretchBlitter::Init(ID3D11Device* dev)
{
    static const char* kShader = R"(
        Texture2D src : register(t0);
        SamplerState smp : register(s0);
        cbuffer C : register(b0) { float4 uvMat; float4 uvOff; float4 invSize; }; // uvOff.zw = dst origin in pixels
        float4 VS(uint id : SV_VertexID) : SV_Position
        {
            float2 p = float2(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0);
            return float4(p, 0, 1);
        }
        float4 PS(float4 pos : SV_Position) : SV_Target
        {
            float2 uv = (pos.xy - uvOff.zw) * invSize.xy;
            uv = float2(dot(uv, uvMat.xy), dot(uv, uvMat.zw)) + uvOff.xy;
            return src.Sample(smp, uv);
        })";
    winrt::com_ptr<ID3DBlob> vs, ps, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, vs.put(), err.put()))) return false;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, ps.put(), err.put()))) return false;
    if (FAILED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, m_Vs.put()))) return false;
    if (FAILED(dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, m_Ps.put()))) return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(dev->CreateSamplerState(&sd, m_Sampler.put()))) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, m_Constants.put()));
}

void StretchBlitter::Blit(ID3D11DeviceContext4* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* rtv, const RECT& dst, int rotation)
{
    if (!m_Vs || !src) return;
    // uv' = M * uv + off, rotating the image clockwise by the monitor's rotation.
    float mat[4] = { 1, 0, 0, 1 };
    float off[2] = { 0, 0 };
    switch (rotation)
    {
    case 2: // 90: uv' = (v, 1-u)
        mat[0] = 0; mat[1] = 1; mat[2] = -1; mat[3] = 0; off[1] = 1;
        break;
    case 3: // 180: uv' = (1-u, 1-v)
        mat[0] = -1; mat[1] = 0; mat[2] = 0; mat[3] = -1; off[0] = 1; off[1] = 1;
        break;
    case 4: // 270: uv' = (1-v, u)
        mat[0] = 0; mat[1] = -1; mat[2] = 1; mat[3] = 0; off[0] = 1;
        break;
    default: break;
    }
    float c[12] = { mat[0], mat[1], mat[2], mat[3], off[0], off[1], (float)dst.left, (float)dst.top,
        1.0f / std::max(1L, dst.right - dst.left), 1.0f / std::max(1L, dst.bottom - dst.top), 0, 0 };
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(m_Constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    memcpy(map.pData, c, sizeof(c));
    ctx->Unmap(m_Constants.get(), 0);

    D3D11_VIEWPORT vp{ (float)dst.left, (float)dst.top, (float)(dst.right - dst.left), (float)(dst.bottom - dst.top), 0, 1 };
    D3D11_RECT sc{ dst.left, dst.top, dst.right, dst.bottom };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetScissorRects(1, &sc);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->VSSetShader(m_Vs.get(), nullptr, 0);
    ctx->PSSetShader(m_Ps.get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[1] = { src };
    ctx->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samps[1] = { m_Sampler.get() };
    ctx->PSSetSamplers(0, 1, samps);
    ID3D11Buffer* cbs[1] = { m_Constants.get() };
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[1] = { nullptr };
    ctx->PSSetShaderResources(0, 1, none);
}

#pragma endregion

#pragma region DrawRegions

void DrawRegions(ID3D11Device5* dev, ID3D11DeviceContext4* ctx, SdShared* s, HANDLE driverProc, std::vector<FrameSource>& src,
    const std::vector<RECT>& regions, ID3D11Texture2D* dst, ID3D11RenderTargetView* rtv, UINT width, UINT height, int targetIndex,
    StretchBlitter* blitter, const int* rotations)
{
    const float black[4] = { 0, 0, 0, 1 };
    if (targetIndex < 0 || targetIndex >= kMaxTargetBuffers) targetIndex = 0;
    for (size_t k = 0; k < src.size(); k++)
    {
        auto& f = src[k];
        auto& m = s->mon[f.index];
        long long key = FrameRingKey(s, f.index);
        if (f.seenKey != key)
        {
            f.seenKey = key;
            f.Refresh(dev, s, driverProc);
        }
        long long seq = SdReadI64(&m.frameSeq);
        f.lastSeenSeq = seq;

        // Region on the panel, clipped to the target size.
        RECT full = regions[k];
        RECT rc = full;
        rc.right = std::min<LONG>(rc.right, (LONG)width);
        rc.bottom = std::min<LONG>(rc.bottom, (LONG)height);
        if (rc.right <= rc.left || rc.bottom <= rc.top) continue;
        D3D11_RECT area{ rc.left, rc.top, rc.right, rc.bottom };

        long long& drawn = f.drawnSeq[targetIndex];
        if (seq > 0 && drawn == seq) continue; // this back buffer already shows exactly this frame
        if (!f.Attached() || seq == 0)
        {
            if (drawn != 0)
            {
                ctx->ClearView(rtv, black, &area, 1);
                drawn = 0;
            }
            continue;
        }
        UINT64 fv = 0;
        int idx = f.Acquire(s, fv);
        if (idx < 0)
        {
            if (drawn < 0) // buffer state unknown: show black rather than garbage
            {
                ctx->ClearView(rtv, black, &area, 1);
                drawn = 0;
            }
            continue; // else keep the previous frame for this pass
        }
        ctx->Wait(f.fence.get(), fv);

        UINT regionW = (UINT)(full.right - full.left), regionH = (UINT)(full.bottom - full.top);
        int rotation = rotations ? rotations[k] : 1;
        bool stretch = f.width != regionW || f.height != regionH || (rotation != 0 && rotation != 1);
        if (stretch && blitter && f.srv[idx])
        {
            blitter->Blit(ctx, f.srv[idx].get(), rtv, rc, rotation);
            drawn = seq;
            continue;
        }

        // Copy path (also the fallback if the blitter is unavailable: crops as before).
        UINT copyW = std::min(f.width, (UINT)(rc.right - rc.left));
        UINT copyH = std::min(f.height, (UINT)(rc.bottom - rc.top));
        bool copyFull = drawn <= 0 || seq - drawn > SD_DIRTY_JOURNAL || stretch;
        SdRect rects[SD_DIRTY_JOURNAL * 4];
        int rectCount = 0;
        UINT64 dirtyArea = 0;
        if (!copyFull)
        {
            for (long long q = drawn + 1; q <= seq && !copyFull; q++)
            {
                SdDirtyFrame e = m.journal[q % SD_DIRTY_JOURNAL];
                // The entry could be mid-rewrite for a frame one journal lap ahead, or
                // already overwritten by it: anything but a clean seq match means copy all.
                if (e.seq != (UINT64)q || e.seqEnd != (UINT64)q || e.count > SD_MAX_DIRTY || e.full)
                {
                    copyFull = true;
                    break;
                }
                for (UINT32 i = 0; i < e.count; i++)
                {
                    SdRect r = e.rects[i];
                    r.left = std::clamp<INT32>(r.left, 0, (INT32)copyW);
                    r.top = std::clamp<INT32>(r.top, 0, (INT32)copyH);
                    r.right = std::clamp<INT32>(r.right, 0, (INT32)copyW);
                    r.bottom = std::clamp<INT32>(r.bottom, 0, (INT32)copyH);
                    if (r.right <= r.left || r.bottom <= r.top) continue;
                    if (rectCount < (int)std::size(rects)) rects[rectCount++] = r;
                    else copyFull = true;
                    dirtyArea += (UINT64)(r.right - r.left) * (r.bottom - r.top);
                }
            }
            if (dirtyArea >= (UINT64)copyW * copyH * 3 / 4) copyFull = true;
        }
        if (copyFull)
        {
            // A buffer that never showed this monitor may hold a bigger previous layout's
            // pixels outside the copied box: black first, once.
            if (drawn < 0) ctx->ClearView(rtv, black, &area, 1);
            D3D11_BOX box{ 0, 0, 0, copyW, copyH, 1 };
            ctx->CopySubresourceRegion(dst, 0, (UINT)rc.left, (UINT)rc.top, 0, f.tex[idx].get(), 0, &box);
        }
        else
        {
            for (int i = 0; i < rectCount; i++)
            {
                const SdRect& r = rects[i];
                D3D11_BOX box{ (UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1 };
                ctx->CopySubresourceRegion(dst, 0, (UINT)(rc.left + r.left), (UINT)(rc.top + r.top), 0, f.tex[idx].get(), 0, &box);
            }
        }
        drawn = seq;
    }
}

#pragma endregion

#pragma region FrameEventWaiter

FrameEventWaiter::FrameEventWaiter(size_t count) : m_Events(count), m_Armed(count)
{
    for (auto& e : m_Events) e.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
}

DWORD FrameEventWaiter::Build(const std::vector<FrameSource>& src, HANDLE* out, DWORD maxOut)
{
    DWORD n = 0;
    for (size_t k = 0; k < src.size() && k < m_Events.size() && n < maxOut; k++)
    {
        auto* f = src[k].fence.get();
        if (!f) continue;
        UINT64 next = f->GetCompletedValue() + 1;
        if (m_Armed[k].first != f || m_Armed[k].second != next)
        {
            if (FAILED(f->SetEventOnCompletion(next, m_Events[k].get()))) continue;
            m_Armed[k] = { f, next };
        }
        out[n++] = m_Events[k].get();
    }
    return n;
}

#pragma endregion
