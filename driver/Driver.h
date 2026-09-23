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

    // Ring of shared textures that the compositor reads from (see protocol.h).
    class FrameRing
    {
    public:
        FrameRing(UINT MonitorIndex, std::shared_ptr<Direct3DDevice> Device);
        ~FrameRing();
        void Publish(ID3D11Texture2D* Source);

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
        UINT m_Width = 0, m_Height = 0;
        DXGI_FORMAT m_Format = DXGI_FORMAT_UNKNOWN;
        bool m_Ready = false;
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
        void PlugMonitors();
        void UnplugMonitors();

        WDFDEVICE m_WdfDevice;
        IDDCX_ADAPTER m_Adapter = nullptr;
        IDDCX_MONITOR m_Monitors[SD_MAX_MONITORS] = {};
        bool m_Plugged = false;
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
