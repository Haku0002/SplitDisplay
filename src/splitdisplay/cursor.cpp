#include "cursor.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

#pragma region CursorSource

CursorSource::~CursorSource()
{
    if (m_Shm) UnmapViewOfFile(m_Shm);
    if (m_Section) CloseHandle(m_Section);
    for (auto& e : m_Events)
        if (e) CloseHandle(e);
}

bool CursorSource::Open()
{
    if (m_Shm) return true;
    m_Section = OpenFileMappingW(FILE_MAP_READ, FALSE, SD_CURSOR_SHM_NAME);
    if (!m_Section) return false;
    m_Shm = (const SdCursorShared*)MapViewOfFile(m_Section, FILE_MAP_READ, 0, 0, sizeof(SdCursorShared));
    if (!m_Shm || m_Shm->magic != SD_CURSOR_MAGIC)
    {
        if (m_Shm) UnmapViewOfFile(m_Shm);
        m_Shm = nullptr;
        CloseHandle(m_Section);
        m_Section = nullptr;
        return false;
    }
    for (int i = 0; i < SD_MAX_MONITORS; i++)
    {
        wchar_t name[96];
        swprintf_s(name, L"%s%d", SD_CURSOR_EVENT_PREFIX, i);
        m_Events[i] = OpenEventW(SYNCHRONIZE, FALSE, name);
    }
    return true;
}

bool CursorSource::Read(int index, State& out, std::vector<BYTE>* shape, UINT knownShapeId)
{
    if (!m_Shm || index < 0 || index >= SD_MAX_MONITORS) return false;
    const SdCursor& c = m_Shm->mon[index];
    for (int attempt = 0; attempt < 64; attempt++)
    {
        LONG s1 = c.seq;
        if (s1 & 1)
        {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        State st;
        st.active = c.active != 0;
        st.visible = c.visible != 0;
        st.x = c.x;
        st.y = c.y;
        st.shapeId = c.shapeId;
        st.type = c.type;
        st.width = std::min<UINT>(c.width, SD_CURSOR_MAX);
        st.height = std::min<UINT>(c.height, SD_CURSOR_MAX);
        st.pitch = std::min<UINT>(c.pitch, SD_CURSOR_MAX * 4);
        st.xHot = c.xHot;
        st.yHot = c.yHot;
        st.seq = s1;
        bool copied = false;
        if (shape && st.visible && st.shapeId != knownShapeId && st.width && st.height && st.pitch)
        {
            shape->assign(c.shape, c.shape + (size_t)st.pitch * st.height);
            copied = true;
        }
        MemoryBarrier();
        if (c.seq == s1)
        {
            out = st;
            if (shape && !copied) shape->clear();
            return true;
        }
    }
    return false;
}

#pragma endregion

#pragma region CursorPainter

bool CursorPainter::Init(ID3D11Device* dev)
{
    static const char* kShader =
        "Texture2D cur : register(t0);\n"
        "SamplerState smp : register(s0);\n"
        "float4 VS(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position\n"
        "{ uv = float2(id & 1, id >> 1); return float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1); }\n"
        "float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return cur.Sample(smp, uv); }\n";
    winrt::com_ptr<ID3DBlob> vs, ps, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, vs.put(), err.put()))) return false;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, ps.put(), err.put()))) return false;
    if (FAILED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, m_Vs.put()))) return false;
    if (FAILED(dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, m_Ps.put()))) return false;
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE; // the texture holds premultiplied alpha (see SetShape)
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, m_Blend.put()))) return false;
    // Exclusion: result = src*(1-dst) + dst*(1-src). With src white this inverts the screen, with
    // src black it leaves it alone -- XOR for the colours cursors use. Destination alpha is kept.
    D3D11_BLEND_DESC xd{};
    auto& xt = xd.RenderTarget[0];
    xt.BlendEnable = TRUE;
    xt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    xt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    xt.BlendOp = D3D11_BLEND_OP_ADD;
    xt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    xt.DestBlendAlpha = D3D11_BLEND_ONE;
    xt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    xt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&xd, m_XorBlend.put()))) return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // drawn 1:1
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    return SUCCEEDED(dev->CreateSamplerState(&sd, m_Sampler.put()));
}

void CursorPainter::SetShape(ID3D11Device* dev, UINT shapeId, UINT type, UINT width, UINT height, UINT pitch, const BYTE* pixels)
{
    if (!width || !height || !pixels) return;
    // Windows converts XOR cursors (text beam, magnifier, legacy monochrome cursors) on its own and
    // hands them over labelled IDDCX_CURSOR_SHAPE_TYPE_ALPHA, but with masked-colour semantics in
    // the alpha byte (0xFF = keep/XOR the screen, 0x00 = draw the colour). Seen on Windows 11 with
    // both FULL and EMULATION XOR support. A real alpha cursor never looks like that: its alpha is
    // not limited to 0/255 with all four corners "opaque black" (that would draw a black box).
    if (type == 2)
    {
        auto at = [&](UINT x, UINT y) { return pixels + (size_t)y * pitch + x * 4; };
        bool binary = true;
        for (UINT y = 0; y < height && binary; y++)
            for (UINT x = 0; x < width && binary; x++)
            {
                BYTE a = at(x, y)[3];
                binary = a == 0 || a == 0xFF;
            }
        auto keep = [&](UINT x, UINT y) {
            const BYTE* p = at(x, y);
            return p[3] == 0xFF && !(p[0] | p[1] | p[2]);
        };
        if (binary && keep(0, 0) && keep(width - 1, 0) && keep(0, height - 1) && keep(width - 1, height - 1)) type = 1;
    }
    // Replace layer: premultiplied BGRA. XOR layer: the colour to XOR with (0 where nothing to do).
    std::vector<UINT32> rep((size_t)width * height), xr((size_t)width * height);
    bool anyXor = false;
    for (UINT y = 0; y < height; y++)
    {
        const BYTE* row = pixels + (size_t)y * pitch;
        for (UINT x = 0; x < width; x++)
        {
            UINT b = row[x * 4 + 0], g = row[x * 4 + 1], r = row[x * 4 + 2], a = row[x * 4 + 3];
            size_t i = (size_t)y * width + x;
            if (type == 1) // IDDCX_CURSOR_SHAPE_TYPE_MASKED_COLOR
            {
                if (a == 0) rep[i] = b | (g << 8) | (r << 16) | 0xFF000000u; // replace: opaque
                else if (b | g | r)
                {
                    xr[i] = b | (g << 8) | (r << 16) | 0xFF000000u; // XOR with this colour
                    anyXor = true;
                }
            }
            else // IDDCX_CURSOR_SHAPE_TYPE_ALPHA: straight alpha
            {
                rep[i] = (b * a / 255) | ((g * a / 255) << 8) | ((r * a / 255) << 16) | (a << 24);
            }
        }
    }
    auto make = [&](const std::vector<UINT32>& px, winrt::com_ptr<ID3D11Texture2D>& tex, winrt::com_ptr<ID3D11ShaderResourceView>& srv) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{ px.data(), width * 4, 0 };
        tex = nullptr;
        srv = nullptr;
        return SUCCEEDED(dev->CreateTexture2D(&td, &init, tex.put())) && SUCCEEDED(dev->CreateShaderResourceView(tex.get(), nullptr, srv.put()));
    };
    winrt::com_ptr<ID3D11Texture2D> tex, xtex;
    winrt::com_ptr<ID3D11ShaderResourceView> srv, xsrv;
    if (!make(rep, tex, srv)) return;
    if (anyXor && !make(xr, xtex, xsrv)) xsrv = nullptr;
    m_Tex = tex;
    m_Srv = srv;
    m_XorTex = xtex;
    m_XorSrv = xsrv;
    m_ShapeId = shapeId;
    m_W = width;
    m_H = height;
}

void CursorPainter::Draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int x, int y, const RECT& clip)
{
    if (!m_Srv || !m_Vs) return;
    D3D11_VIEWPORT vp{ (float)x, (float)y, (float)m_W, (float)m_H, 0, 1 };
    D3D11_RECT sc{ std::max<LONG>(x, clip.left), std::max<LONG>(y, clip.top), std::min<LONG>(x + (LONG)m_W, clip.right),
        std::min<LONG>(y + (LONG)m_H, clip.bottom) };
    if (sc.right <= sc.left || sc.bottom <= sc.top) return;
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetScissorRects(1, &sc);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const float factor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(m_Blend.get(), factor, 0xFFFFFFFF);
    ctx->VSSetShader(m_Vs.get(), nullptr, 0);
    ctx->PSSetShader(m_Ps.get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[1] = { m_Srv.get() };
    ctx->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samps[1] = { m_Sampler.get() };
    ctx->PSSetSamplers(0, 1, samps);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->Draw(4, 0);
    if (m_XorSrv && m_XorBlend)
    {
        ctx->OMSetBlendState(m_XorBlend.get(), factor, 0xFFFFFFFF);
        ID3D11ShaderResourceView* xs[1] = { m_XorSrv.get() };
        ctx->PSSetShaderResources(0, 1, xs);
        ctx->Draw(4, 0);
    }
    ctx->OMSetBlendState(nullptr, factor, 0xFFFFFFFF);
    ID3D11ShaderResourceView* none[1] = { nullptr };
    ctx->PSSetShaderResources(0, 1, none);
}

#pragma endregion
