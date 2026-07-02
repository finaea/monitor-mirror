// ===========================================================================
// mirror.cpp - Ultra-low-latency monitor mirror (Windows, D3D11 / DXGI)
//
// Mirrors a user-chosen rectangle of a "working" monitor (where your drawing
// software runs) onto a rectangle of a second "mirror" monitor. The mirror
// rectangle is locked to the working rectangle's aspect ratio, so the picture
// is never distorted - it is copied 1:1 when the sizes match, or cleanly scaled
// when they differ.
//
// Everything is configured from a visual GUI (see config_gui.cpp), reached from
// the tray "Edit..." menu or shown automatically on first launch / on any
// error. The configuration is saved next to the exe (mirror.cfg) and can be
// imported / exported.
//
// Architecture (all-GPU, no CPU round-trip):
//   - DXGI Desktop Duplication gives the working monitor as a GPU texture.
//   - CopySubresourceRegion crops the working rectangle into a GPU texture.
//   - A flip-model swapchain presents it with tearing (vsync off), the
//     compositor (DWM) bypassed via independent flip -> minimal latency.
//   - The cursor (delivered separately by Duplication) is composited as a quad.
//   - Runs from the system tray; left-click toggles mirroring.
//
// Build: see build.bat  (MinGW g++).
// ===========================================================================
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <mmsystem.h>
#include <vector>
#include <string>
#include <cstdio>
#include <algorithm>

#include "monitors.h"
#include "config.h"
#include "config_gui.h"

using Microsoft::WRL::ComPtr;

// ----------------------------------------------------------------------------
// Globals / tray
// ----------------------------------------------------------------------------
static const wchar_t* kClassName = L"MirrorD3D11Window";
static const UINT WM_TRAY   = WM_APP + 1;
static const UINT IDM_TOGGLE = 1001;
static const UINT IDM_EDIT   = 1002;
static const UINT IDM_QUIT   = 1003;

static HINSTANCE   g_hInst = nullptr;
static HWND        g_hwnd = nullptr;
static NOTIFYICONDATAW g_nid = {};
static HICON       g_icon = nullptr;
static bool        g_enabled = true;
static bool        g_running = true;
static bool        g_reconfigure = false;   // rebuild pipeline (display change / edit-save)
static bool        g_showConfig  = false;   // open the setup dialog next loop iteration

static MirrorConfig            g_cfg;
static std::vector<MonitorInfo> g_monitors;
static std::wstring            g_cfgPath;

// ----------------------------------------------------------------------------
static void logf(const char* fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    OutputDebugStringA(buf); OutputDebugStringA("\n");
}

// ----------------------------------------------------------------------------
// Renderer + capture state
// ----------------------------------------------------------------------------
struct Mirror {
    ComPtr<ID3D11Device>        dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIAdapter1>       srcAdapter;
    ComPtr<IDXGIOutput1>        srcOutput;
    ComPtr<IDXGIOutputDuplication> dupl;

    ComPtr<IDXGISwapChain2>     swap;
    HANDLE                      waitable = nullptr;
    bool                        allowTearing = false;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D>     cropTex;
    ComPtr<ID3D11ShaderResourceView> cropSRV;

    ComPtr<ID3D11VertexShader>  vs;
    ComPtr<ID3D11PixelShader>   ps;
    ComPtr<ID3D11Buffer>        cb;
    ComPtr<ID3D11SamplerState>  sampPoint, sampLinear;
    ComPtr<ID3D11BlendState>    blendOpaque, blendAlpha;
    ComPtr<ID3D11RasterizerState> rsScissor;

    // cursor
    ComPtr<ID3D11Texture2D>     curTex;
    ComPtr<ID3D11ShaderResourceView> curSRV;
    int  curW = 0, curH = 0, curX = 0, curY = 0;
    bool curVisible = false;

    // geometry (derived from config), all in physical pixels
    int cropX=0, cropY=0, cropW=0, cropH=0;   // region on the working monitor
    int winW=0, winH=0;                        // window == full mirror monitor
    int drawX=0, drawY=0, drawW=0, drawH=0;    // where the mirror is drawn in the window
    bool vsync=false, showCursor=true;
    bool scaling=false;

    bool init(const MirrorConfig& cfg, const std::vector<MonitorInfo>& mons, HWND hwnd, std::wstring& err);
    bool createDuplication();
    bool createPipeline();
    void renderFrame();
    void updateCursor(IDXGIOutputDuplication* d, const DXGI_OUTDUPL_FRAME_INFO& fi);
    void buildCursorTexture(const std::vector<BYTE>& bgra, int w, int h);
};

struct CB { float posRect[4]; float uvRect[4]; };

static const char* kVS =
"cbuffer CB:register(b0){float4 posRect;float4 uvRect;};"
"struct VO{float4 pos:SV_Position;float2 uv:TEXCOORD;};"
"VO main(uint vid:SV_VertexID){"
" float2 g=float2((vid&1),(vid>>1));"
" VO o;"
" o.pos=float4(lerp(posRect.x,posRect.z,g.x),lerp(posRect.y,posRect.w,g.y),0,1);"
" o.uv =float2(lerp(uvRect.x,uvRect.z,g.x),lerp(uvRect.y,uvRect.w,g.y));"
" return o;}";
static const char* kPS =
"Texture2D tex:register(t0);SamplerState smp:register(s0);"
"struct VO{float4 pos:SV_Position;float2 uv:TEXCOORD;};"
"float4 main(VO i):SV_Target{return tex.Sample(smp,i.uv);}";

// ----------------------------------------------------------------------------
// Build all geometry + D3D resources from the config. Returns false + reason.
// ----------------------------------------------------------------------------
bool Mirror::init(const MirrorConfig& cfg, const std::vector<MonitorInfo>& mons, HWND hwnd, std::wstring& err) {
    int si = FindMonitor(mons, cfg.source.device);
    int ti = FindMonitor(mons, cfg.target.device);
    if (si < 0 || ti < 0) { err = L"A configured monitor is not connected."; return false; }

    // ---- find the DXGI output matching the working monitor's device name ----
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf()))) {
        err = L"Direct3D initialization failed (CreateDXGIFactory1)."; return false; }
    ComPtr<IDXGIAdapter1> ad;
    bool found = false;
    for (UINT ai = 0; !found && factory->EnumAdapters1(ai, ad.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++ai) {
        ComPtr<IDXGIOutput> o;
        for (UINT oi = 0; ad->EnumOutputs(oi, o.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC d; o->GetDesc(&d);
            if (!d.AttachedToDesktop) continue;
            if (cfg.source.device == d.DeviceName) {
                srcAdapter = ad; o.As(&srcOutput); found = true; break;
            }
        }
    }
    if (!found || !srcOutput) { err = L"Could not open the working monitor for capture."; return false; }

    // ---- geometry ----
    // cover=true : the window fills the whole mirror monitor, mirror drawn as a
    //              sub-rectangle with black margins around it.
    // cover=false: the window is exactly the mirror rectangle, so the rest of the
    //              monitor (desktop / other apps) stays visible.
    cropX = cfg.source.x; cropY = cfg.source.y; cropW = cfg.source.w; cropH = cfg.source.h;
    drawW = cfg.target.w; drawH = cfg.target.h;
    if (cfg.cover) {
        winW = mons[ti].w(); winH = mons[ti].h();
        drawX = cfg.target.x; drawY = cfg.target.y;
    } else {
        winW = cfg.target.w; winH = cfg.target.h;
        drawX = 0; drawY = 0;
    }
    vsync = cfg.vsync;     showCursor = cfg.cursor;
    scaling = (cropW != drawW) || (cropH != drawH);
    logf("crop=(%d,%d %dx%d)  win=%dx%d  draw=(%d,%d %dx%d)  scaling=%d",
         cropX, cropY, cropW, cropH, winW, winH, drawX, drawY, drawW, drawH, scaling);

    // ---- device on the source adapter ----
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(srcAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION, dev.GetAddressOf(), &fl, ctx.GetAddressOf()))) {
        err = L"Direct3D device creation failed."; return false; }

    if (!createDuplication()) { err = L"Screen capture (Desktop Duplication) could not start on the working monitor."; return false; }
    if (!createPipeline())    { err = L"The graphics pipeline could not be created."; return false; }
    return true;
}

bool Mirror::createDuplication() {
    dupl.Reset();
    return SUCCEEDED(srcOutput->DuplicateOutput(dev.Get(), dupl.GetAddressOf()));
}

bool Mirror::createPipeline() {
    ComPtr<IDXGIFactory2> f2;
    ComPtr<IDXGIDevice>   dxgiDev; dev.As(&dxgiDev);
    ComPtr<IDXGIAdapter>  ad;      dxgiDev->GetAdapter(ad.GetAddressOf());
    ad->GetParent(__uuidof(IDXGIFactory2), (void**)f2.GetAddressOf());

    ComPtr<IDXGIFactory5> f5; f2.As(&f5);
    BOOL tearing = FALSE;
    if (f5) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    allowTearing = (tearing == TRUE);

    DXGI_SWAP_CHAIN_DESC1 sc = {};
    sc.Width = winW; sc.Height = winH;
    sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.Scaling = DXGI_SCALING_NONE;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (allowTearing) sc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(f2->CreateSwapChainForHwnd(dev.Get(), g_hwnd, &sc, nullptr, nullptr, sc1.GetAddressOf())))
        return false;
    f2->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(sc1.As(&swap))) return false;
    swap->SetMaximumFrameLatency(1);
    waitable = swap->GetFrameLatencyWaitableObject();

    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)bb.GetAddressOf()))) return false;
    if (FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, rtv.GetAddressOf()))) return false;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = cropW; td.Height = cropH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, cropTex.GetAddressOf()))) return false;
    if (FAILED(dev->CreateShaderResourceView(cropTex.Get(), nullptr, cropSRV.GetAddressOf()))) return false;

    ComPtr<ID3DBlob> vsb, psb, e;
    if (FAILED(D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, vsb.GetAddressOf(), e.GetAddressOf()))) return false;
    e.Reset();
    if (FAILED(D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, psb.GetAddressOf(), e.GetAddressOf()))) return false;
    if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, vs.GetAddressOf()))) return false;
    if (FAILED(dev->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, ps.GetAddressOf()))) return false;

    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, cb.GetAddressOf()))) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(dev->CreateSamplerState(&sd, sampPoint.GetAddressOf()))) return false;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(dev->CreateSamplerState(&sd, sampLinear.GetAddressOf()))) return false;

    D3D11_BLEND_DESC bo = {};
    bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bo, blendOpaque.GetAddressOf()))) return false;
    D3D11_BLEND_DESC ba = {};
    auto& rt = ba.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;  rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;   rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&ba, blendAlpha.GetAddressOf()))) return false;

    // rasterizer with scissor enabled so the (scaled) mirror + cursor stay
    // clipped to the draw rectangle, leaving the rest of the monitor black.
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE; rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, rsScissor.GetAddressOf()))) return false;

    return true;
}

void Mirror::buildCursorTexture(const std::vector<BYTE>& bgra, int w, int h) {
    curTex.Reset(); curSRV.Reset();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd = {}; srd.pSysMem = bgra.data(); srd.SysMemPitch = w * 4;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &srd, curTex.GetAddressOf())))
        dev->CreateShaderResourceView(curTex.Get(), nullptr, curSRV.GetAddressOf());
    curW = w; curH = h;
}

void Mirror::updateCursor(IDXGIOutputDuplication* d, const DXGI_OUTDUPL_FRAME_INFO& fi) {
    if (fi.LastMouseUpdateTime.QuadPart != 0) {
        curVisible = fi.PointerPosition.Visible;
        curX = fi.PointerPosition.Position.x;
        curY = fi.PointerPosition.Position.y;
    }
    if (fi.PointerShapeBufferSize == 0) return;

    std::vector<BYTE> buf(fi.PointerShapeBufferSize);
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si = {};
    UINT got = 0;
    if (FAILED(d->GetFramePointerShape(fi.PointerShapeBufferSize, buf.data(), &got, &si))) return;

    int w = si.Width, h = si.Height;
    std::vector<BYTE> out;
    if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        out.resize(w * h * 4);
        for (int y = 0; y < h; ++y)
            memcpy(&out[y * w * 4], &buf[y * si.Pitch], w * 4);
    } else if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        out.resize(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            int s = y * si.Pitch + x * 4, o = (y * w + x) * 4;
            BYTE b = buf[s+0], g = buf[s+1], r = buf[s+2], mask = buf[s+3];
            if (mask == 0)            { out[o]=b; out[o+1]=g; out[o+2]=r; out[o+3]=255; }
            else if (r | g | b)       { out[o]=b; out[o+1]=g; out[o+2]=r; out[o+3]=255; }
            else                      { out[o]=0; out[o+1]=0; out[o+2]=0; out[o+3]=0; }
        }
    } else { // MONOCHROME
        h = si.Height / 2;
        out.resize(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            int bit = 7 - (x % 8);
            int andB = (buf[y * si.Pitch + (x / 8)] >> bit) & 1;
            int xorB = (buf[(h + y) * si.Pitch + (x / 8)] >> bit) & 1;
            BYTE v = (andB == 0) ? (xorB ? 255 : 0) : 0;
            BYTE a = (andB == 0) ? 255 : 0;
            int o = (y*w + x)*4; out[o+0]=v; out[o+1]=v; out[o+2]=v; out[o+3]=a;
        }
    }
    if (!out.empty()) buildCursorTexture(out, w, h);
}

void Mirror::renderFrame() {
    ComPtr<IDXGIResource> res;
    DXGI_OUTDUPL_FRAME_INFO fi = {};
    HRESULT hr = dupl->AcquireNextFrame(15, &fi, res.GetAddressOf());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        if (!createDuplication()) { g_reconfigure = true; }   // monitor likely gone
        return;
    }
    if (FAILED(hr)) return;

    ComPtr<ID3D11Texture2D> frameTex; res.As(&frameTex);
    // clamp crop box to the actual frame in case the resolution changed under us
    D3D11_TEXTURE2D_DESC fd = {}; if (frameTex) frameTex->GetDesc(&fd);
    if (frameTex && (int)fd.Width >= cropX + cropW && (int)fd.Height >= cropY + cropH) {
        D3D11_BOX box = { (UINT)cropX, (UINT)cropY, 0, (UINT)(cropX+cropW), (UINT)(cropY+cropH), 1 };
        ctx->CopySubresourceRegion(cropTex.Get(), 0, 0, 0, 0, frameTex.Get(), 0, &box);
    }

    if (showCursor) updateCursor(dupl.Get(), fi);
    dupl->ReleaseFrame();

    // ---- render ----
    float black[4] = {0,0,0,1};
    ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    ctx->ClearRenderTargetView(rtv.Get(), black);   // letterbox / margins stay black

    D3D11_VIEWPORT vp = { 0, 0, (float)winW, (float)winH, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    D3D11_RECT scissor = { drawX, drawY, drawX + drawW, drawY + drawH };
    ctx->RSSetScissorRects(1, &scissor);
    ctx->RSSetState(rsScissor.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11SamplerState* smp = (scaling ? sampLinear : sampPoint).Get();
    ctx->PSSetSamplers(0, 1, &smp);
    float bf[4] = {0,0,0,0};

    // window-pixel rect -> NDC
    auto pxQuad = [&](float x, float y, float w, float h, ID3D11ShaderResourceView* srv, ID3D11BlendState* bs){
        float x0 =  (x        / winW) * 2.f - 1.f;
        float x1 = ((x + w)   / winW) * 2.f - 1.f;
        float y0 = 1.f - (y        / winH) * 2.f;
        float y1 = 1.f - ((y + h)  / winH) * 2.f;
        D3D11_MAPPED_SUBRESOURCE ms; ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        CB c = { {x0,y0,x1,y1}, {0,0,1,1} }; memcpy(ms.pData, &c, sizeof(c)); ctx->Unmap(cb.Get(), 0);
        ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->OMSetBlendState(bs, bf, 0xffffffff);
        ctx->Draw(4, 0);
    };

    // base: the mirrored region, scaled into the draw rectangle
    pxQuad((float)drawX, (float)drawY, (float)drawW, (float)drawH, cropSRV.Get(), blendOpaque.Get());

    // cursor: map from source pixels into the draw rectangle (same scale)
    if (showCursor && curVisible && curSRV && curW > 0 && cropW > 0 && cropH > 0) {
        float sx = (float)drawW / cropW, sy = (float)drawH / cropH;
        float rx = (curX - cropX) * sx + drawX;
        float ry = (curY - cropY) * sy + drawY;
        float rw = curW * sx, rh = curH * sy;
        if (rx + rw > drawX && ry + rh > drawY && rx < drawX + drawW && ry < drawY + drawH)
            pxQuad(rx, ry, rw, rh, curSRV.Get(), blendAlpha.Get());
    }

    UINT flags = (!vsync && allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    swap->Present(vsync ? 1 : 0, flags);
}

// ----------------------------------------------------------------------------
// Tray
// ----------------------------------------------------------------------------
static HICON makeTrayIcon(bool on) {
    const int S = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi); bi.bV5Width = S; bi.bV5Height = -S;
    bi.bV5Planes = 1; bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    HDC hdc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    DWORD* px = (DWORD*)bits;

    const DWORD CLEAR = 0x00000000;
    const DWORD FRAME = 0xFF202833;
    const DWORD DIM   = on ? 0xFF14323A : 0xFF2A2A2A;
    const DWORD LIT   = on ? 0xFF35D6F0 : 0xFF555555;
    for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) {
        DWORD c = CLEAR;
        bool frame = (x >= 2 && x <= 29 && y >= 8 && y <= 23);
        bool inner = (x >= 4 && x <= 27 && y >= 10 && y <= 21);
        bool stand = (x >= 14 && x <= 17 && y >= 24 && y <= 26);
        if (frame) c = FRAME;
        if (inner) c = (x >= 16) ? LIT : DIM;
        if (stand) c = FRAME;
        px[y * S + x] = c;
    }
    std::vector<BYTE> zero((S * S) / 8, 0);
    HBITMAP mask = CreateBitmap(S, S, 1, 1, zero.data());
    ICONINFO ii = {}; ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return ic;
}

static void addTray(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_icon = makeTrayIcon(true);
    g_nid.hIcon = g_icon;
    wcscpy(g_nid.szTip, L"Mirror: ON (left-click to disable)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void removeTray() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_icon) { DestroyIcon(g_icon); g_icon = nullptr; }
}
static void setEnabled(bool on) {
    g_enabled = on;
    ShowWindow(g_hwnd, on ? SW_SHOWNA : SW_HIDE);
    HICON old = g_icon;
    g_icon = makeTrayIcon(on);
    g_nid.uFlags = NIF_ICON | NIF_TIP;
    g_nid.hIcon = g_icon;
    wcscpy(g_nid.szTip, on ? L"Mirror: ON (left-click to disable)"
                          : L"Mirror: OFF (left-click to enable)");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    if (old) DestroyIcon(old);
}
static void showMenu(HWND hwnd) {
    POINT p; GetCursorPos(&p);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_enabled ? MF_CHECKED : 0), IDM_TOGGLE, L"Enabled");
    AppendMenuW(menu, MF_STRING, IDM_EDIT, L"Edit setup...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP)      setEnabled(!g_enabled);
        else if (LOWORD(lp) == WM_RBUTTONUP) showMenu(hwnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_TOGGLE)      setEnabled(!g_enabled);
        else if (LOWORD(wp) == IDM_EDIT)   g_showConfig = true;
        else if (LOWORD(wp) == IDM_QUIT) { g_running = false; PostQuitMessage(0); }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { g_running = false; PostQuitMessage(0); }
        return 0;
    case WM_DISPLAYCHANGE:
        g_reconfigure = true;   // resolution / monitor topology changed
        return 0;
    case WM_DESTROY:
        g_running = false; PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ----------------------------------------------------------------------------
// Setup / fallback: make sure g_cfg is valid and the Mirror is built.
// Repeatedly shows the setup dialog until the user succeeds or chooses Quit.
// Returns false if the user chose Cancel & Quit.
// ----------------------------------------------------------------------------
static bool ensureConfigured(Mirror& m, std::wstring banner) {
    for (;;) {
        g_monitors = EnumMonitors();
        std::wstring err = ValidateConfig(g_cfg, g_monitors);
        if (err.empty()) {
            // position the window (whole monitor when covering, else just the
            // mirror rectangle), then build
            int ti = FindMonitor(g_monitors, g_cfg.target.device);
            const MonitorInfo& tm = g_monitors[ti];
            int wx, wy, ww, wh;
            if (g_cfg.cover) {
                wx = tm.rect.left; wy = tm.rect.top; ww = tm.w(); wh = tm.h();
            } else {
                wx = tm.rect.left + g_cfg.target.x; wy = tm.rect.top + g_cfg.target.y;
                ww = g_cfg.target.w; wh = g_cfg.target.h;
            }
            SetWindowPos(g_hwnd, HWND_TOPMOST, wx, wy, ww, wh, SWP_NOACTIVATE);
            m = Mirror();
            std::wstring berr;
            if (m.init(g_cfg, g_monitors, g_hwnd, berr)) {
                if (g_enabled) ShowWindow(g_hwnd, SW_SHOWNA);
                return true;
            }
            err = berr;
        }
        std::wstring b = !banner.empty() ? banner : err;
        if (RunConfigDialog(g_cfg, g_monitors, g_hInst, b, g_cfgPath) == ConfigResult::Quit)
            return false;
        banner.clear();   // subsequent rounds show the real validation error
    }
}

// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    g_hInst = hInst;
    typedef BOOL (WINAPI* SetCtx)(HANDLE);
    if (auto p = (SetCtx)GetProcAddress(GetModuleHandleW(L"user32"), "SetProcessDpiAwarenessContext"))
        p((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */);
    timeBeginPeriod(1);   // finer Sleep granularity for the FPS cap

    g_cfgPath = DefaultConfigPath();
    g_monitors = EnumMonitors();

    // ---- first launch / load: prompt to set up or import if missing/invalid ----
    bool loaded = LoadConfig(g_cfg, g_cfgPath);
    std::wstring initialBanner;
    bool needPrompt = false;
    if (!loaded) {
        initialBanner = L"No configuration found.  Set up your working area below, or click Import... to load one.  "
                        L"The app won't start until it's configured.";
        needPrompt = true;
    } else {
        std::wstring err = ValidateConfig(g_cfg, g_monitors);
        if (!err.empty()) { initialBanner = err + L"   Please re-setup or Import a configuration."; needPrompt = true; }
    }
    if (needPrompt) {
        if (RunConfigDialog(g_cfg, g_monitors, hInst, initialBanner, g_cfgPath) == ConfigResult::Quit) {
            timeEndPeriod(1);
            return 0;   // user chose Cancel & Quit
        }
    }

    // ---- window (covers the mirror monitor; repositioned in ensureConfigured) ----
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kClassName, L"mirror",
        WS_POPUP, 0, 0, 640, 480, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) { MessageBoxW(nullptr, L"CreateWindow failed", L"mirror", MB_ICONERROR); timeEndPeriod(1); return 1; }

    Mirror m;
    if (!ensureConfigured(m, L"")) { timeEndPeriod(1); return 0; }
    addTray(g_hwnd);

    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    LARGE_INTEGER last = {}; QueryPerformanceCounter(&last);

    MSG msg;
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!g_running) break;

        // tray "Edit setup..." -> open the dialog live
        if (g_showConfig) {
            g_showConfig = false;
            MirrorConfig edited = g_cfg;
            if (RunConfigDialog(edited, g_monitors, hInst, L"", g_cfgPath) == ConfigResult::Save) {
                g_cfg = edited; g_reconfigure = true;
            }
        }

        // rebuild after an edit-save or a display change (with fallback prompts)
        if (g_reconfigure) {
            g_reconfigure = false;
            if (!ensureConfigured(m, L"")) { g_running = false; break; }
        }

        if (g_enabled) {
            if (m.waitable) WaitForSingleObjectEx(m.waitable, 100, TRUE);
            if (g_cfg.fps > 0) {
                double minSec = 1.0 / g_cfg.fps;
                LARGE_INTEGER now; QueryPerformanceCounter(&now);
                double dt = double(now.QuadPart - last.QuadPart) / freq.QuadPart;
                if (dt < minSec) {
                    DWORD ms = (DWORD)((minSec - dt) * 1000.0);
                    if (ms > 0) Sleep(ms);
                }
                QueryPerformanceCounter(&last);
            }
            m.renderFrame();
        } else {
            WaitMessage();
        }
    }

    removeTray();
    timeEndPeriod(1);
    return 0;
}
