#pragma once
// Consumer side of the driver's frame rings: attaching to shared textures, deciding what
// actually needs copying for a given back buffer (whole region, dirty rects only, a
// stretched blit for sub-native/rotated modes, or nothing), and waiting for new frames.

#include <Windows.h>
#include <d3d11_4.h>
#include <winrt/base.h>

#include <utility>
#include <vector>

#include "protocol.h"

// Back buffers a compositor rotates through (exclusive: 2 primaries, window: 3 swap buffers).
constexpr int kMaxTargetBuffers = 3;

struct FrameSource
{
    int index = 0;          // virtual monitor index
    long long attachedKey = -1; // (driverEpoch, handleGeneration) of the handles we hold, -1 = none
    long long seenKey = -1;     // last driver key we tried to attach to (even if that failed)
    winrt::com_ptr<ID3D11Texture2D> tex[SD_BUFFERS];
    winrt::com_ptr<ID3D11ShaderResourceView> srv[SD_BUFFERS];
    winrt::com_ptr<ID3D11Fence> fence;
    UINT width = 0, height = 0;
    long long lastSeenSeq = -1;                    // newest frameSeq observed (change detection)
    long long drawnSeq[kMaxTargetBuffers] = { -1, -1, -1 }; // what each back buffer shows: -1 unknown, 0 black, >0 that frame
    bool reserved = false;

    bool Attached() const { return attachedKey >= 0; }
    void Drop();
    bool Refresh(ID3D11Device5* dev, SdShared* s, HANDLE driverProc);
    int Acquire(SdShared* s, UINT64& fenceValue); // ring buffer now reserved for reading, or -1
    void Release(SdShared* s);
};

// (driverEpoch, handleGeneration) packed so a restarted driver (generation counter reset)
// can never compare equal to handles held from its previous incarnation.
inline long long FrameRingKey(const SdShared* s, int index)
{
    return ((long long)(unsigned int)s->driverEpoch << 32) | (unsigned int)s->mon[index].handleGeneration;
}

// True if any source has a new ring or a newer frame than last time DrawRegions looked.
// Compares with the state last tried, not the one attached: a monitor whose ring cannot be
// opened (switched off, or on another GPU) must not count as changed on every pass.
bool FramesChanged(SdShared* s, const std::vector<FrameSource>& src);

// Fullscreen-triangle blit that stretches (and optionally rotates) a source texture into a
// region of the render target. Only used when a virtual monitor runs below its region size
// (a scaled mode picked in a game) or rotated; the normal path is a plain copy.
class StretchBlitter
{
public:
    bool Init(ID3D11Device* dev);
    // rotation is a DISPLAYCONFIG_ROTATION value (1 identity, 2/3/4 = 90/180/270).
    void Blit(ID3D11DeviceContext4* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* rtv, const RECT& dst, int rotation);

private:
    winrt::com_ptr<ID3D11VertexShader> m_Vs;
    winrt::com_ptr<ID3D11PixelShader> m_Ps;
    winrt::com_ptr<ID3D11SamplerState> m_Sampler;
    winrt::com_ptr<ID3D11Buffer> m_Constants;
};

// Copies the newest frame of every split monitor into its region of `dst` for back buffer
// number `targetIndex`, skipping whatever that buffer already shows and copying only the
// rects that changed since (see the journal in protocol.h). Black where a monitor has no
// frame. The copies hold ring reservations until the caller releases them after its fence.
// rotations[k] is the DISPLAYCONFIG_ROTATION of monitor k (0/1 = identity), or nullptr.
void DrawRegions(ID3D11Device5* dev, ID3D11DeviceContext4* ctx, SdShared* s, HANDLE driverProc, std::vector<FrameSource>& src,
    const std::vector<RECT>& regions, ID3D11Texture2D* dst, ID3D11RenderTargetView* rtv, UINT width, UINT height, int targetIndex,
    StretchBlitter* blitter, const int* rotations);

// Arms one event per source on the next fence value the driver will signal, so a compositor
// can sleep until a new frame finishes instead of polling. Each event is armed once per
// frame; a wait that stays pending is not registered again.
class FrameEventWaiter
{
public:
    explicit FrameEventWaiter(size_t count);
    // Fills `out` with the events to wait on and returns how many (0 = no source attached).
    DWORD Build(const std::vector<FrameSource>& src, HANDLE* out, DWORD maxOut);

private:
    std::vector<winrt::handle> m_Events;
    std::vector<std::pair<ID3D11Fence*, UINT64>> m_Armed;
};
