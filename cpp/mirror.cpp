// ===========================================================================
// mirror.cpp - Ultra-low-latency 1:1 monitor mirror (Windows, D3D11 / DXGI)
//
//   SOURCE : 32:9, native 5120x1440 (DISPLAY1, primary)
//   TARGET : 16:9, 2560x1440      (DISPLAY2)
//   Mirrors the RIGHT 2560x1440 half of the source onto the target, 1:1.
//
// Architecture (all-GPU, no CPU round-trip):
//   - DXGI Desktop Duplication gives the desktop as a GPU texture.
//   - CopySubresourceRegion crops the right half into a GPU texture.
//   - A flip-model swapchain presents it with tearing (vsync OFF), the
//     compositor (DWM) bypassed via independent flip -> minimal latency.
//   - The cursor (which Duplication delivers separately) is composited as a
//     GPU quad on top.
//   - Runs from the system tray; left-click or the menu toggles mirroring.
//
// Build: see build.bat  (MinGW g++).   Quit: tray menu, or Esc when focused.
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
#include <vector>
#include <string>
#include <cstdio>

using Microsoft::WRL::ComPtr;

// ----------------------------------------------------------------------------
// CONFIG
// ----------------------------------------------------------------------------
static int   SRC_W = 5120, SRC_H = 1440;   // native source resolution
static int   TGT_W = 2560, TGT_H = 1440;   // target resolution
static int   CROP_X = 0, CROP_Y = 0;       // set after detection (right half)
static const bool VSYNC = false;           // false = tearing/lowest latency
static const bool SHOW_CURSOR = true;
// CROP_X defaults to (SRC_W - TGT_W) i.e. the right half (computed in main).

// ----------------------------------------------------------------------------
// Globals / tray
// ----------------------------------------------------------------------------
static const wchar_t* kClassName = L"MirrorD3D11Window";
static const UINT WM_TRAY = WM_APP + 1;
static const UINT IDM_TOGGLE = 1001;
static const UINT IDM_QUIT   = 1002;

static HWND        g_hwnd = nullptr;
static NOTIFYICONDATAW g_nid = {};
static HICON       g_icon = nullptr;
static bool        g_enabled = true;   // mirroring on/off
static bool        g_running = true;

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
#define HRCHECK(expr, msg) do { HRESULT _hr=(expr); if(FAILED(_hr)){ \
    wchar_t _b[256]; swprintf(_b,256,L"%S failed: 0x%08lX",msg,(unsigned long)_hr); \
    MessageBoxW(nullptr,_b,L"mirror",MB_ICONERROR); return false; } } while(0)

static void log(const char* fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    OutputDebugStringA(buf); OutputDebugStringA("\n");
    printf("%s\n", buf); fflush(stdout);
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
    DXGI_OUTPUT_DESC            srcDesc = {};

    ComPtr<IDXGISwapChain2>     swap;
    HANDLE                      waitable = nullptr;
    bool                        allowTearing = false;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D>     cropTex;      // SRV-able copy of the crop region
    ComPtr<ID3D11ShaderResourceView> cropSRV;

    // shader pipeline
    ComPtr<ID3D11VertexShader>  vs;
    ComPtr<ID3D11PixelShader>   ps;
    ComPtr<ID3D11Buffer>        cb;
    ComPtr<ID3D11SamplerState>  samp;
    ComPtr<ID3D11BlendState>    blendOpaque, blendAlpha;

    // cursor
    ComPtr<ID3D11Texture2D>     curTex;
    ComPtr<ID3D11ShaderResourceView> curSRV;
    int  curW = 0, curH = 0;
    int  curX = 0, curY = 0;
    bool curVisible = false;

    bool init();
    bool createDuplication();
    bool createPipeline();
    void renderFrame();   // acquire -> crop -> draw -> present (one iteration)
    void updateCursor(IDXGIOutputDuplication* d, const DXGI_OUTDUPL_FRAME_INFO& fi);
    void buildCursorTexture(const std::vector<BYTE>& bgra, int w, int h);
};

struct CB { float posRect[4]; float uvRect[4]; };

// Embedded HLSL: one quad generator + one textured pixel shader.
static const char* kVS =
"cbuffer CB:register(b0){float4 posRect;float4 uvRect;};"
"struct VO{float4 pos:SV_Position;float2 uv:TEXCOORD;};"
"VO main(uint vid:SV_VertexID){"
" float2 g=float2((vid&1),(vid>>1));"          // 0:(0,0) 1:(1,0) 2:(0,1) 3:(1,1)
" VO o;"
" o.pos=float4(lerp(posRect.x,posRect.z,g.x),lerp(posRect.y,posRect.w,g.y),0,1);"
" o.uv =float2(lerp(uvRect.x,uvRect.z,g.x),lerp(uvRect.y,uvRect.w,g.y));"
" return o;}";

static const char* kPS =
"Texture2D tex:register(t0);SamplerState smp:register(s0);"
"struct VO{float4 pos:SV_Position;float2 uv:TEXCOORD;};"
"float4 main(VO i):SV_Target{return tex.Sample(smp,i.uv);}";

// ----------------------------------------------------------------------------
// Detection: find the source output (== SRC_W x SRC_H, else widest) and the
// target monitor (== TGT_W x TGT_H, not the source). Returns target rect.
// ----------------------------------------------------------------------------
static bool detectDisplays(Mirror& m, RECT& targetRect) {
    ComPtr<IDXGIFactory1> factory;
    HRCHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf()),
            "CreateDXGIFactory1");

    struct Found { ComPtr<IDXGIAdapter1> ad; ComPtr<IDXGIOutput1> out; DXGI_OUTPUT_DESC desc; };
    std::vector<Found> outs;
    ComPtr<IDXGIAdapter1> ad;
    for (UINT ai = 0; factory->EnumAdapters1(ai, ad.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++ai) {
        ComPtr<IDXGIOutput> o;
        for (UINT oi = 0; ad->EnumOutputs(oi, o.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC d; o->GetDesc(&d);
            if (!d.AttachedToDesktop) continue;
            ComPtr<IDXGIOutput1> o1; o.As(&o1);
            outs.push_back({ad, o1, d});
        }
    }
    if (outs.empty()) { MessageBoxW(nullptr, L"No outputs found", L"mirror", MB_ICONERROR); return false; }

    auto wOf = [](const DXGI_OUTPUT_DESC& d){ return d.DesktopCoordinates.right - d.DesktopCoordinates.left; };
    auto hOf = [](const DXGI_OUTPUT_DESC& d){ return d.DesktopCoordinates.bottom - d.DesktopCoordinates.top; };

    // source: exact match, else widest
    int srcIdx = -1;
    for (size_t i = 0; i < outs.size(); ++i)
        if (wOf(outs[i].desc) == SRC_W && hOf(outs[i].desc) == SRC_H) { srcIdx = (int)i; break; }
    if (srcIdx < 0) {
        int best = -1, bw = -1;
        for (size_t i = 0; i < outs.size(); ++i) if (wOf(outs[i].desc) > bw) { bw = wOf(outs[i].desc); best = (int)i; }
        srcIdx = best;
        SRC_W = wOf(outs[srcIdx].desc); SRC_H = hOf(outs[srcIdx].desc);
        log("source exact match not found; using widest output %dx%d", SRC_W, SRC_H);
    }
    m.srcAdapter = outs[srcIdx].ad;
    m.srcOutput  = outs[srcIdx].out;
    m.srcDesc    = outs[srcIdx].desc;

    // target: TGT_W x TGT_H, not the source; else any other output
    int tgtIdx = -1;
    for (size_t i = 0; i < outs.size(); ++i)
        if ((int)i != srcIdx && wOf(outs[i].desc) == TGT_W && hOf(outs[i].desc) == TGT_H) { tgtIdx = (int)i; break; }
    if (tgtIdx < 0)
        for (size_t i = 0; i < outs.size(); ++i) if ((int)i != srcIdx) { tgtIdx = (int)i; break; }
    if (tgtIdx < 0) { MessageBoxW(nullptr, L"No second monitor for the mirror window", L"mirror", MB_ICONERROR); return false; }

    targetRect = outs[tgtIdx].desc.DesktopCoordinates;
    TGT_W = targetRect.right - targetRect.left;
    TGT_H = targetRect.bottom - targetRect.top;

    if (CROP_X == 0 && CROP_Y == 0) CROP_X = SRC_W - TGT_W;   // default: right half
    if (CROP_X + TGT_W > SRC_W) CROP_X = SRC_W - TGT_W;
    if (CROP_Y + TGT_H > SRC_H) CROP_Y = SRC_H - TGT_H;

    log("source %dx%d  target %dx%d @(%ld,%ld)  crop=(%d,%d)",
        SRC_W, SRC_H, TGT_W, TGT_H, (long)targetRect.left, (long)targetRect.top, CROP_X, CROP_Y);
    return true;
}

// ----------------------------------------------------------------------------
bool Mirror::init() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl;
    // device MUST be on the source adapter (same GPU drives both monitors here)
    HRCHECK(D3D11CreateDevice(srcAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION, dev.GetAddressOf(), &fl, ctx.GetAddressOf()),
            "D3D11CreateDevice");
    if (!createDuplication()) return false;
    if (!createPipeline())    return false;
    return true;
}

bool Mirror::createDuplication() {
    dupl.Reset();
    HRESULT hr = srcOutput->DuplicateOutput(dev.Get(), dupl.GetAddressOf());
    if (FAILED(hr)) {
        wchar_t b[256]; swprintf(b, 256, L"DuplicateOutput failed: 0x%08lX", (unsigned long)hr);
        MessageBoxW(nullptr, b, L"mirror", MB_ICONERROR); return false;
    }
    return true;
}

bool Mirror::createPipeline() {
    // ---- swapchain (flip model, tearing, waitable) ----
    ComPtr<IDXGIFactory2> f2;
    ComPtr<IDXGIDevice>   dxgiDev; dev.As(&dxgiDev);
    ComPtr<IDXGIAdapter>  ad;      dxgiDev->GetAdapter(ad.GetAddressOf());
    ad->GetParent(__uuidof(IDXGIFactory2), (void**)f2.GetAddressOf());

    ComPtr<IDXGIFactory5> f5; f2.As(&f5);
    BOOL tearing = FALSE;
    if (f5) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    allowTearing = (tearing == TRUE);

    DXGI_SWAP_CHAIN_DESC1 sc = {};
    sc.Width = TGT_W; sc.Height = TGT_H;
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
    HRCHECK(f2->CreateSwapChainForHwnd(dev.Get(), g_hwnd, &sc, nullptr, nullptr, sc1.GetAddressOf()),
            "CreateSwapChainForHwnd");
    f2->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    HRCHECK(sc1.As(&swap), "QI IDXGISwapChain2");
    swap->SetMaximumFrameLatency(1);
    waitable = swap->GetFrameLatencyWaitableObject();

    // ---- backbuffer RTV (flip model: buffer 0 is always current) ----
    ComPtr<ID3D11Texture2D> bb;
    HRCHECK(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)bb.GetAddressOf()), "GetBuffer");
    HRCHECK(dev->CreateRenderTargetView(bb.Get(), nullptr, rtv.GetAddressOf()), "CreateRTV");

    // ---- SRV-able crop texture (copy target from the duplicated frame) ----
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = TGT_W; td.Height = TGT_H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRCHECK(dev->CreateTexture2D(&td, nullptr, cropTex.GetAddressOf()), "CreateCropTex");
    HRCHECK(dev->CreateShaderResourceView(cropTex.Get(), nullptr, cropSRV.GetAddressOf()), "CropSRV");

    // ---- shaders ----
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, vsb.GetAddressOf(), err.GetAddressOf()))) {
        MessageBoxA(nullptr, err ? (char*)err->GetBufferPointer() : "VS compile failed", "mirror", MB_ICONERROR); return false; }
    err.Reset();
    if (FAILED(D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, psb.GetAddressOf(), err.GetAddressOf()))) {
        MessageBoxA(nullptr, err ? (char*)err->GetBufferPointer() : "PS compile failed", "mirror", MB_ICONERROR); return false; }
    HRCHECK(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, vs.GetAddressOf()), "CreateVS");
    HRCHECK(dev->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, ps.GetAddressOf()), "CreatePS");

    // ---- constant buffer ----
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRCHECK(dev->CreateBuffer(&bd, nullptr, cb.GetAddressOf()), "CreateCB");

    // ---- sampler (point = exact 1:1, no blur) ----
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    HRCHECK(dev->CreateSamplerState(&sd, samp.GetAddressOf()), "CreateSampler");

    // ---- blend states ----
    D3D11_BLEND_DESC bo = {};
    bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    HRCHECK(dev->CreateBlendState(&bo, blendOpaque.GetAddressOf()), "BlendOpaque");
    D3D11_BLEND_DESC ba = {};
    auto& rt = ba.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;  rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;   rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    HRCHECK(dev->CreateBlendState(&ba, blendAlpha.GetAddressOf()), "BlendAlpha");

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
            memcpy(&out[y * w * 4], &buf[y * si.Pitch], w * 4);            // BGRA straight
    } else if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        // Mask byte semantics are INVERTED vs normal alpha:
        //   mask==0    -> opaque, copy RGB
        //   mask==0xFF -> XOR with screen. Black (RGB==0) XOR == no-op == transparent.
        //                 Non-black XOR is a true inversion (rare); approximate as opaque.
        out.resize(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            int s = y * si.Pitch + x * 4, o = (y * w + x) * 4;
            BYTE b = buf[s+0], g = buf[s+1], r = buf[s+2], mask = buf[s+3];
            if (mask == 0)            { out[o]=b; out[o+1]=g; out[o+2]=r; out[o+3]=255; }
            else if (r | g | b)       { out[o]=b; out[o+1]=g; out[o+2]=r; out[o+3]=255; }
            else                      { out[o]=0; out[o+1]=0; out[o+2]=0; out[o+3]=0; }
        }
    } else { // MONOCHROME: top half AND mask, bottom half XOR mask, 1bpp
        h = si.Height / 2;
        out.resize(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            int bit = 7 - (x % 8);
            int andB = (buf[y * si.Pitch + (x / 8)] >> bit) & 1;
            int xorB = (buf[(h + y) * si.Pitch + (x / 8)] >> bit) & 1;
            // and==0 -> opaque black/white; and==1 -> transparent (invert approximated as transparent)
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
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;          // no new frame; keep last on screen
    if (hr == DXGI_ERROR_ACCESS_LOST) { createDuplication(); return; }
    if (FAILED(hr)) return;

    ComPtr<ID3D11Texture2D> frameTex; res.As(&frameTex);
    // crop the right-half region straight into our SRV texture (GPU->GPU)
    D3D11_BOX box = { (UINT)CROP_X, (UINT)CROP_Y, 0, (UINT)(CROP_X+TGT_W), (UINT)(CROP_Y+TGT_H), 1 };
    if (frameTex) ctx->CopySubresourceRegion(cropTex.Get(), 0, 0, 0, 0, frameTex.Get(), 0, &box);

    if (SHOW_CURSOR) updateCursor(dupl.Get(), fi);
    dupl->ReleaseFrame();

    // ---- render: base blit + optional cursor quad ----
    ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)TGT_W, (float)TGT_H, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->PSSetSamplers(0, 1, samp.GetAddressOf());
    float bf[4] = {0,0,0,0};

    auto drawQuad = [&](float x0,float y0,float x1,float y1, ID3D11ShaderResourceView* srv, ID3D11BlendState* bs){
        D3D11_MAPPED_SUBRESOURCE ms; ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        CB c = { {x0,y0,x1,y1}, {0,0,1,1} }; memcpy(ms.pData, &c, sizeof(c)); ctx->Unmap(cb.Get(), 0);
        ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->OMSetBlendState(bs, bf, 0xffffffff);
        ctx->Draw(4, 0);
    };

    // base: fullscreen (NDC: x -1..1, y +1..-1), uv 0..1
    drawQuad(-1.f, 1.f, 1.f, -1.f, cropSRV.Get(), blendOpaque.Get());

    // cursor: map source-pixel position into the window, draw at native size
    if (SHOW_CURSOR && curVisible && curSRV && curW > 0) {
        float cx = (float)(curX - CROP_X), cy = (float)(curY - CROP_Y);
        if (cx + curW > 0 && cy + curH > 0 && cx < TGT_W && cy < TGT_H) {
            float x0 =  (cx / TGT_W) * 2.f - 1.f;
            float x1 = ((cx + curW) / TGT_W) * 2.f - 1.f;
            float y0 = 1.f - (cy / TGT_H) * 2.f;
            float y1 = 1.f - ((cy + curH) / TGT_H) * 2.f;
            drawQuad(x0, y0, x1, y1, curSRV.Get(), blendAlpha.Get());
        }
    }

    UINT flags = (!VSYNC && allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    swap->Present(VSYNC ? 1 : 0, flags);
}

// ----------------------------------------------------------------------------
// Tray
// ----------------------------------------------------------------------------
// Generate a 32x32 ARGB tray icon at runtime: an ultrawide screen with its
// right half lit (the mirrored region). No external .ico file needed.
static HICON makeTrayIcon(bool on) {
    const int S = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi); bi.bV5Width = S; bi.bV5Height = -S;  // top-down
    bi.bV5Planes = 1; bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    HDC hdc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    DWORD* px = (DWORD*)bits;

    const DWORD CLEAR = 0x00000000;
    const DWORD FRAME = 0xFF202833;                  // dark bezel
    const DWORD DIM   = on ? 0xFF14323A : 0xFF2A2A2A; // left half (source)
    const DWORD LIT   = on ? 0xFF35D6F0 : 0xFF555555; // right half (mirrored)
    for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) {
        DWORD c = CLEAR;
        bool frame = (x >= 2 && x <= 29 && y >= 8 && y <= 23);     // wide 32:9-ish screen
        bool inner = (x >= 4 && x <= 27 && y >= 10 && y <= 21);
        bool stand = (x >= 14 && x <= 17 && y >= 24 && y <= 26);   // little stand
        if (frame) c = FRAME;
        if (inner) c = (x >= 16) ? LIT : DIM;                      // right half lit
        if (stand) c = FRAME;
        px[y * S + x] = c;
    }
    std::vector<BYTE> zero((S * S) / 8, 0);          // all-zero AND mask (alpha governs)
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
    g_icon = makeTrayIcon(on);                  // lit when ON, greyed when OFF
    g_nid.uFlags = NIF_ICON | NIF_TIP;
    g_nid.hIcon = g_icon;
    wcscpy(g_nid.szTip, on ? L"Mirror: ON (left-click to disable)"
                          : L"Mirror: OFF (left-click to enable)");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;   // restore for future adds
    if (old) DestroyIcon(old);
}

static void showMenu(HWND hwnd) {
    POINT p; GetCursorPos(&p);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_enabled ? MF_CHECKED : 0), IDM_TOGGLE, L"Enabled");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");
    SetForegroundWindow(hwnd);                          // so the menu dismisses correctly
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
        if (LOWORD(wp) == IDM_TOGGLE) setEnabled(!g_enabled);
        else if (LOWORD(wp) == IDM_QUIT) { g_running = false; PostQuitMessage(0); }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { g_running = false; PostQuitMessage(0); }
        return 0;
    case WM_DESTROY:
        g_running = false; PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Per-Monitor-V2 DPI awareness -> DXGI/coords are physical pixels (1:1).
    typedef BOOL (WINAPI* SetCtx)(HANDLE);
    if (auto p = (SetCtx)GetProcAddress(GetModuleHandleW(L"user32"), "SetProcessDpiAwarenessContext"))
        p((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */);

    Mirror m;
    RECT tr;
    if (!detectDisplays(m, tr)) return 1;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    // borderless window covering the target monitor
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kClassName, L"mirror",
        WS_POPUP, tr.left, tr.top, TGT_W, TGT_H, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) { MessageBoxW(nullptr, L"CreateWindow failed", L"mirror", MB_ICONERROR); return 1; }

    if (!m.init()) return 1;
    addTray(g_hwnd);
    ShowWindow(g_hwnd, SW_SHOWNA);

    // Single-threaded loop: pump messages, then render a frame when enabled.
    MSG msg;
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!g_running) break;
        if (g_enabled) {
            if (m.waitable) WaitForSingleObjectEx(m.waitable, 100, TRUE);  // pace to present
            m.renderFrame();                                               // AcquireNextFrame paces capture
        } else {
            WaitMessage();   // idle: sleep until a tray message arrives
        }
    }

    removeTray();
    return 0;
}
