#pragma once
// Hardware cursor, compositor side: reads the cursor the driver takes over from the OS
// (SdCursorShared, see protocol.h) and draws it on top of the composed panel image.
//
// The cursor is drawn per present, after the content, into the target that is about to be shown:
// content and cursor never mix in any texture that is kept from frame to frame, so a cursor move
// costs one small redraw instead of a frame from every virtual monitor.

#include <Windows.h>
#include <d3d11_4.h>
#include <winrt/base.h>

#include <vector>

#include "protocol.h"

class CursorSource
{
public:
    ~CursorSource();
    bool Open(); // false if the driver has no cursor section (older driver)
    bool Opened() const { return m_Shm != nullptr; }

    struct State
    {
        bool active = false; // the OS hands this monitor's cursor to us
        bool visible = false;
        int x = 0, y = 0; // image top-left, monitor pixels
        UINT shapeId = 0, type = 0, width = 0, height = 0, pitch = 0, xHot = 0, yHot = 0;
        LONG seq = 0;
    };
    // Consistent snapshot of monitor `index`. If the shape differs from knownShapeId, copies it too.
    bool Read(int index, State& out, std::vector<BYTE>* shape, UINT knownShapeId);
    HANDLE Event(int index) const { return index >= 0 && index < SD_MAX_MONITORS ? m_Events[index] : nullptr; }

private:
    HANDLE m_Section = nullptr;
    const SdCursorShared* m_Shm = nullptr;
    HANDLE m_Events[SD_MAX_MONITORS] = {};
};

// Draws one cursor image. Alpha cursors are alpha-blended. Masked-colour cursors (the text beam,
// the magnifier, most legacy and app cursors) carry a per-pixel mask: 0x00 = replace the screen
// pixel with the colour, 0xFF = XOR the screen pixel with it (black = leave it, white = invert).
// They are drawn in two passes: the replace pixels alpha-blended, then the XOR pixels with an
// "exclusion" blend, src*(1-dst) + dst*(1-src), which is exact for the black/white XOR pixels
// these cursors use.
class CursorPainter
{
public:
    bool Init(ID3D11Device* dev);
    // Uploads a new shape. Pixels: 32 bpp, IDDCX_CURSOR_SHAPE_TYPE in `type`.
    void SetShape(ID3D11Device* dev, UINT shapeId, UINT type, UINT width, UINT height, UINT pitch, const BYTE* pixels);
    // Draws the current shape with its top-left at (x, y) in the render target, clipped to `clip`.
    void Draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int x, int y, const RECT& clip);
    UINT ShapeId() const { return m_ShapeId; }
    UINT Width() const { return m_W; }
    UINT Height() const { return m_H; }
    bool HasShape() const { return m_Srv != nullptr; }

private:
    winrt::com_ptr<ID3D11VertexShader> m_Vs;
    winrt::com_ptr<ID3D11PixelShader> m_Ps;
    winrt::com_ptr<ID3D11BlendState> m_Blend;
    winrt::com_ptr<ID3D11BlendState> m_XorBlend;
    winrt::com_ptr<ID3D11Texture2D> m_XorTex;
    winrt::com_ptr<ID3D11ShaderResourceView> m_XorSrv; // null if the cursor has no XOR pixels
    winrt::com_ptr<ID3D11SamplerState> m_Sampler;
    winrt::com_ptr<ID3D11Texture2D> m_Tex;
    winrt::com_ptr<ID3D11ShaderResourceView> m_Srv;
    UINT m_ShapeId = 0, m_W = 0, m_H = 0;
};
