#pragma once

#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_4.h>
#include <avrt.h>
#include <wrl.h>
#include <sddl.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "protocol.h"
#include "ring.h"

namespace SplitDisplay
{
    void DLog(const wchar_t* fmt, ...);

    struct Direct3DDevice
    {
        explicit Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid) {}
        HRESULT Init();

        LUID AdapterLuid;
        Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
        Microsoft::WRL::ComPtr<ID3D11Device5> Device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> DeviceContext;
    };

    // Ring of shared textures that the compositor reads from (see protocol.h). Only the parts
    // of the desktop that changed are copied: each slot accumulates the dirty rects of every
    // frame since it was last written, because its contents are that old.
    class FrameRing
    {
    public:
        FrameRing(UINT MonitorIndex, std::shared_ptr<Direct3DDevice> Device);
        ~FrameRing();
        void Publish(ID3D11Texture2D* Source, IDDCX_SWAPCHAIN SwapChain, const IDDCX_METADATA& Meta);

    private:
        HRESULT Recreate(const D3D11_TEXTURE2D_DESC& Desc);
        void Release();

        UINT m_Index;
        std::shared_ptr<Direct3DDevice> m_Device;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Tex[SD_BUFFERS];
        Microsoft::WRL::ComPtr<ID3D11Fence> m_Fence;
        HANDLE m_TexHandle[SD_BUFFERS] = {};
        HANDLE m_FenceHandle = nullptr;
        UINT64 m_FenceValue = 0;
        UINT64 m_Seq = 0; // frames published by this ring (== shared frameSeq)
        UINT m_Width = 0, m_Height = 0;
        DXGI_FORMAT m_Format = DXGI_FORMAT_UNKNOWN;
        bool m_Ready = false;
        std::vector<RECT> m_Accum[SD_BUFFERS]; // dirty rects each slot is still missing
        bool m_AccumFull[SD_BUFFERS] = { true, true, true };
    };

    class SwapChainProcessor
    {
    public:
        SwapChainProcessor(UINT MonitorIndex, IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
        ~SwapChainProcessor();

    private:
        static DWORD CALLBACK RunThread(LPVOID Argument);
        void Run();
        void RunCore();

        UINT m_Index;
        IDDCX_SWAPCHAIN m_hSwapChain;
        std::shared_ptr<Direct3DDevice> m_Device;
        HANDLE m_hAvailableBufferEvent;
        Microsoft::WRL::Wrappers::HandleT<Microsoft::WRL::Wrappers::HandleTraits::HANDLENullTraits> m_hThread;
        Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
    };

    class IndirectDeviceContext
    {
    public:
        IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
        virtual ~IndirectDeviceContext();

        void InitAdapter();
        void FinishInit();

    private:
        static DWORD CALLBACK CommandThread(LPVOID Argument);
        void CommandLoop();
        void ReconcileMonitors(const SdConfig& cfg); // plug/unplug/replace slots to match cfg
        void UnplugMonitors();
        void DepartSlot(UINT index);
        bool CreateSlot(UINT index, const SdConfig& cfg);
        void UpdateModes(const SdConfig& cfg); // IddCxMonitorUpdateModes on every plugged monitor
        void ReleaseOwnership();               // give back the one-owner claim on the shared state

        WDFDEVICE m_WdfDevice;
        IDDCX_ADAPTER m_Adapter = nullptr;
        IDDCX_MONITOR m_Monitors[SD_MAX_MONITORS] = {};
        SdMonitorConfig m_Current[SD_MAX_MONITORS] = {}; // what each plugged slot was created with
        bool m_Plugged = false;
        bool m_Owner = false;
        LUID m_RenderAdapter = {};
        LONG m_LastModesGeneration = 0;
        HANDLE m_Section = nullptr;
        HANDLE m_CmdEvent = nullptr;
        HANDLE m_StopEvent = nullptr;
        HANDLE m_Thread = nullptr;
    };

    class IndirectMonitorContext
    {
    public:
        IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor, UINT Index);
        virtual ~IndirectMonitorContext();

        void AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
        void UnassignSwapChain();
        UINT Index() const { return m_Index; }

    private:
        IDDCX_MONITOR m_Monitor;
        UINT m_Index;
        std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
    };
}
