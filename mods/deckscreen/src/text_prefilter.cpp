// text_prefilter.cpp - crisp small text: each line texture prefiltered to the size it is drawn at.
//
// The game draws each line of text (FUN_005a1750) from a texture it wrote the line into on the CPU,
// 48 texels per line, shrunk to the font size on screen - 18 px in the Load menu, 2.67x - with plain
// bilinear filtering (its sampler is trilinear, but the textures have one mip). Bilinear at 2.67x
// reads a few of the texels under each pixel, so strokes come out uneven.
//
// Mipmaps were tried first (docs/MODS.md, "Deckscreen"): even with alpha-weighted and
// Lanczos-filtered levels the text stayed soft, because trilinear filtering blends the half- and the
// quarter-size level, and the renderer's sampler (so its LOD bias) could not be reached.
//
// This does what the manager's icon pack does for the icons, per line and at runtime. The quad the game
// draws a line with gives its texel -> pixel mapping (u = u0 + (x - x0) * sx, likewise v; x0/y0 on
// whole pixels). Each texel of a shadow texture is filled with the Lanczos-2 downscale of the line for
// the screen pixel it falls in, i.e. the line pre-shrunk and stretched back nearest-neighbour. The
// game's bilinear sample for a pixel then sits between two texels holding that same value and returns
// it exactly. That needs sx, sy >= 2 (below that neighbouring pixels share texels); lines drawn larger
// keep the game's own texture. Coverage can be lifted a little ("weight", stem darkening).
//
// The line textures are 1024x64 B8G8R8A8, USAGE_DYNAMIC + CPU write (Map/Unmap), so the shadow is a
// separate DEFAULT texture, rebuilt whenever the game writes the line (Unmap, or a copy/update into
// it) on that same call - the thread the game drives the context from - and the Phyre texture's view
// (+0x2c) points at the shadow's. Off, the game's own views return.
#include "text_prefilter.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace text_prefilter {
namespace {

LogFn g_log = nullptr;
HMODULE g_d3d11 = nullptr;
const AtmtModApi* g_api = nullptr;
const int32_t* g_setting = nullptr;
const float* g_weight = nullptr;
float g_weight_seen = 0.0f;
LONG g_enabled_seen = -1;

constexpr size_t kViewOffset = 0x2c;
constexpr int kMaxEntries = 512;
constexpr float kMinShrink = 2.0f;

struct Mapping {
    float u0, v0;  // the texel coordinate at the quad's corner
    float sx, sy;  // texels per pixel
    float x0, y0;  // the corner's pixel (only its fraction matters)
};

struct Entry {
    uint8_t* object;                      // the Phyre texture
    ID3D11ShaderResourceView* original;   // the game's view (a reference held)
    ID3D11Resource* source;               // the dynamic texture it shows (a reference held)
    ID3D11Texture2D* copy;                // level 0 as the game wrote it (shader input)
    ID3D11ShaderResourceView* copy_view;
    ID3D11Texture2D* shadow;              // the prefiltered line
    ID3D11ShaderResourceView* shadow_view;
    ID3D11RenderTargetView* shadow_target;
    UINT width, height;
    Mapping map;
    bool usable;  // shrunk at least kMinShrink both ways
    bool dirty;
    bool swapped;
};

Entry g_entries[kMaxEntries];
int g_count = 0;
int g_next_evict = 0;
volatile LONG g_dirty = 0;
volatile LONG g_refreshes = 0;
CRITICAL_SECTION g_lock;
__thread int g_inside = 0;  // our own copies go through the hooked calls too

ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_params = nullptr;
bool g_shader_failed = false;

const char kShader[] = R"(
Texture2D<float4> src : register(t0);
cbuffer Params : register(b0) {
    float2 origin;   // texel coordinate at the quad's corner
    float2 shrink;   // texels per pixel
    float2 corner;   // the corner's pixel fraction
    float weight;
    float unused;
};
void vs(uint id : SV_VertexID, out float4 pos : SV_Position) {
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float lanczos2(float x) {
    x = abs(x);
    if (x >= 2) return 0;
    if (x < 1e-4) return 1;
    float px = 3.14159265 * x;
    return 2 * sin(px) * sin(px * 0.5) / (px * px);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    uint w, h;
    src.GetDimensions(w, h);
    // The screen pixel this texel falls in, and that pixel's centre in texels.
    float2 pixel = floor((pos.xy - origin) / shrink + corner);
    float2 centre = origin + (pixel + 0.5 - corner) * shrink;
    float2 reach = shrink * 2;
    int2 lo = max(int2(floor(centre - reach)), 0);
    int2 hi = min(int2(ceil(centre + reach)), int2(w, h));
    float3 rgb = 0;
    float a = 0;
    float total = 0;
    for (int y = lo.y; y < hi.y; ++y) {
        float wy = lanczos2((y + 0.5 - centre.y) / shrink.y);
        for (int x = lo.x; x < hi.x; ++x) {
            float k = wy * lanczos2((x + 0.5 - centre.x) / shrink.x);
            float4 t = src.Load(int3(x, y, 0));
            rgb += t.rgb * t.a * k;
            a += t.a * k;
            total += k;
        }
    }
    float coverage = saturate(a / max(total, 1e-4));
    coverage = 1 - pow(1 - coverage, weight);   // stem darkening
    return float4(a > 1e-4 ? saturate(rgb / a) : 0, coverage);
}
)";

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (p == nullptr || VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
    return static_cast<const uint8_t*>(p) + n <= static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

bool IsD3D11(const void* object) {
    const auto* vtable = static_cast<const void* const*>(object);
    if (!Readable(vtable, sizeof(void*))) return false;
    HMODULE owner = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(vtable[0]), &owner) &&
           owner == g_d3d11;
}

template <typename T>
void SafeRelease(T*& p) {
    if (p != nullptr) p->Release();
    p = nullptr;
}

ID3D11ShaderResourceView*& ViewOf(uint8_t* object) {
    return *reinterpret_cast<ID3D11ShaderResourceView**>(object + kViewOffset);
}

void ReleaseShadow(Entry& e) {
    SafeRelease(e.shadow_target);
    SafeRelease(e.shadow_view);
    SafeRelease(e.shadow);
    SafeRelease(e.copy_view);
    SafeRelease(e.copy);
}

void Evict(Entry& e) {
    if (e.swapped && ViewOf(e.object) == e.shadow_view) ViewOf(e.object) = e.original;
    ReleaseShadow(e);
    e.original->Release();
    e.source->Release();
    e = Entry{};
}

bool CompileShaders(ID3D11Device* device) {
    if (g_vs != nullptr) return true;
    if (g_shader_failed) return false;
    g_shader_failed = true;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (compile == nullptr) {
        g_log("text_prefilter: d3dcompiler_47.dll/D3DCompile not found");
        return false;
    }
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(compile(kShader, sizeof(kShader) - 1, "text_prefilter", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs, &errors)) ||
        FAILED(compile(kShader, sizeof(kShader) - 1, "text_prefilter", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps, &errors))) {
        g_log("text_prefilter: shader compile failed: %s",
              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        SafeRelease(vs);
        SafeRelease(ps);
        SafeRelease(errors);
        return false;
    }
    const bool ok = SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
                    SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps));
    vs->Release();
    ps->Release();
    D3D11_BUFFER_DESC cb{32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    if (!ok || FAILED(device->CreateBuffer(&cb, nullptr, &g_params))) {
        g_log("text_prefilter: could not create the shaders");
        SafeRelease(g_vs);
        SafeRelease(g_ps);
        SafeRelease(g_params);
        return false;
    }
    g_shader_failed = false;
    g_log("text_prefilter: shaders ready");
    return true;
}

bool CreateShadow(ID3D11Device* device, Entry& e) {
    D3D11_TEXTURE2D_DESC desc;
    static_cast<ID3D11Texture2D*>(e.source)->GetDesc(&desc);
    e.width = desc.Width;
    e.height = desc.Height;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &e.copy)) ||
        FAILED(device->CreateShaderResourceView(e.copy, nullptr, &e.copy_view))) {
        return false;
    }
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &e.shadow)) ||
        FAILED(device->CreateShaderResourceView(e.shadow, nullptr, &e.shadow_view)) ||
        FAILED(device->CreateRenderTargetView(e.shadow, nullptr, &e.shadow_target))) {
        return false;
    }
    e.shadow_view->AddRef();  // the reference the Phyre object releases if it frees itself
    return true;
}

// Draws the prefiltered line into e.shadow, leaving every piece of pipeline state as the game had it.
void Prefilter(ID3D11DeviceContext* c, Entry& e) {
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11ShaderResourceView* ps_view = nullptr;
    ID3D11Buffer* ps_params = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blend_factor[4];
    UINT sample_mask;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencil_ref;
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* depth_view = nullptr;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->IAGetInputLayout(&layout);
    c->IAGetPrimitiveTopology(&topology);
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->GSGetShader(&gs, nullptr, nullptr);
    c->HSGetShader(&hs, nullptr, nullptr);
    c->DSGetShader(&ds, nullptr, nullptr);
    c->PSGetShaderResources(0, 1, &ps_view);
    c->PSGetConstantBuffers(0, 1, &ps_params);
    c->RSGetState(&raster);
    c->OMGetBlendState(&blend, blend_factor, &sample_mask);
    c->OMGetDepthStencilState(&depth, &stencil_ref);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth_view);
    c->RSGetViewports(&viewport_count, viewports);

    const float weight = g_weight != nullptr && *g_weight > 0.0f ? *g_weight : 1.0f;
    const float corner_x = e.map.x0 - std::floor(e.map.x0);
    const float corner_y = e.map.y0 - std::floor(e.map.y0);
    const float params[8] = {e.map.u0, e.map.v0, e.map.sx, e.map.sy, corner_x, corner_y, weight, 0};
    c->UpdateSubresource(g_params, 0, nullptr, params, 0, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g_vs, nullptr, 0);
    c->PSSetShader(g_ps, nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->RSSetState(nullptr);
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &g_params);
    c->OMSetRenderTargets(1, &e.shadow_target, nullptr);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
    c->RSSetViewports(1, &vp);
    c->PSSetShaderResources(0, 1, &e.copy_view);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    c->PSSetShaderResources(0, 1, &none);

    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth_view);
    c->RSSetViewports(viewport_count, viewports);
    c->OMSetDepthStencilState(depth, stencil_ref);
    c->OMSetBlendState(blend, blend_factor, sample_mask);
    c->RSSetState(raster);
    c->PSSetShaderResources(0, 1, &ps_view);
    c->PSSetConstantBuffers(0, 1, &ps_params);
    c->DSSetShader(ds, nullptr, 0);
    c->HSSetShader(hs, nullptr, 0);
    c->GSSetShader(gs, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->VSSetShader(vs, nullptr, 0);
    c->IASetPrimitiveTopology(topology);
    c->IASetInputLayout(layout);
    for (auto* t : targets) SafeRelease(t);
    SafeRelease(depth_view);
    SafeRelease(depth);
    SafeRelease(blend);
    SafeRelease(raster);
    SafeRelease(ps_params);
    SafeRelease(ps_view);
    SafeRelease(ds);
    SafeRelease(hs);
    SafeRelease(gs);
    SafeRelease(ps);
    SafeRelease(vs);
    SafeRelease(layout);
}

// Context thread: the game just wrote `resource` (or nullptr: nothing in particular).
void Refresh(ID3D11DeviceContext* context, ID3D11Resource* resource) {
    if (resource == nullptr && g_dirty == 0) return;
    EnterCriticalSection(&g_lock);
    const bool enabled = g_setting != nullptr && *g_setting != 0;
    for (int i = 0; i < g_count; ++i) {
        Entry& e = g_entries[i];
        if (e.object == nullptr) continue;
        if (e.source == resource) e.dirty = true;
        if (!e.dirty || !enabled || !e.usable) continue;
        e.dirty = false;
        if (e.shadow == nullptr) {
            ID3D11Device* device = nullptr;
            context->GetDevice(&device);
            const bool ok = CompileShaders(device) && CreateShadow(device, e);
            device->Release();
            if (!ok) {
                g_log("text_prefilter: could not create a shadow for %p", static_cast<void*>(e.source));
                ReleaseShadow(e);
                e.usable = false;
                continue;
            }
        }
        ++g_inside;
        context->CopyResource(e.copy, e.source);
        Prefilter(context, e);
        --g_inside;
        InterlockedIncrement(&g_refreshes);
        if (!e.swapped && ViewOf(e.object) == e.original) {
            ViewOf(e.object) = e.shadow_view;
            e.swapped = true;
        }
    }
    InterlockedExchange(&g_dirty, 0);
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------- context hooks
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using CopyRegionFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                              ID3D11Resource*, UINT, const D3D11_BOX*);
using CopyFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using UpdateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                          const void*, UINT, UINT);
UnmapFn g_real_unmap = nullptr;
CopyRegionFn g_real_copy_region = nullptr;
CopyFn g_real_copy = nullptr;
UpdateFn g_real_update = nullptr;

void STDMETHODCALLTYPE UnmapDetour(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    g_real_unmap(c, r, sub);
    if (!g_inside) Refresh(c, r);
}
void STDMETHODCALLTYPE CopyRegionDetour(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT sub, UINT x, UINT y,
                                        UINT z, ID3D11Resource* src, UINT src_sub, const D3D11_BOX* box) {
    g_real_copy_region(c, dst, sub, x, y, z, src, src_sub, box);
    if (!g_inside) Refresh(c, dst);
}
void STDMETHODCALLTYPE CopyDetour(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    g_real_copy(c, dst, src);
    if (!g_inside) Refresh(c, dst);
}
void STDMETHODCALLTYPE UpdateDetour(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box,
                                    const void* data, UINT row, UINT depth) {
    g_real_update(c, dst, sub, box, data, row, depth);
    if (!g_inside) Refresh(c, dst);
}

bool g_hooked = false;

// The immediate context, from the device a text texture's view belongs to.
bool HookContext(ID3D11ShaderResourceView* view) {
    ID3D11Device* device = nullptr;
    view->GetDevice(&device);
    if (device == nullptr) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    device->Release();
    if (context == nullptr) return false;
    context->Release();  // the game keeps it alive
    void* const* vtable = *reinterpret_cast<void* const* const*>(context);
    struct { int slot; void* detour; void** real; } hooks[] = {
        {15, reinterpret_cast<void*>(&UnmapDetour), reinterpret_cast<void**>(&g_real_unmap)},
        {46, reinterpret_cast<void*>(&CopyRegionDetour), reinterpret_cast<void**>(&g_real_copy_region)},
        {47, reinterpret_cast<void*>(&CopyDetour), reinterpret_cast<void**>(&g_real_copy)},
        {48, reinterpret_cast<void*>(&UpdateDetour), reinterpret_cast<void**>(&g_real_update)},
    };
    for (const auto& h : hooks) {
        void* handle = g_api->hook_create(vtable[h.slot], h.detour, h.real);
        if (handle == nullptr || g_api->hook_enable(handle) != 0) {
            g_log("text_prefilter: could not hook context slot %d", h.slot);
            return false;
        }
    }
    g_log("text_prefilter: context %p hooked", static_cast<void*>(context));
    return true;
}

float AsFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// The line's texel -> pixel mapping from the quad FUN_005a1750 draws it with: 4 vertices of 7 dwords,
// u, v in texels (float) then x, y in pixels (int) at +4/+5; vertex 0 and vertex 3 are opposite
// corners. The quad is placed by a translation (whole pixels in practice), not included here, which
// is why only the fraction of x0/y0 is used.
bool ReadMapping(const void* quad_ptr, Mapping* out) {
    const auto* q = static_cast<const uint32_t*>(quad_ptr);
    if (!Readable(q, 28 * 4)) return false;
    const float u0 = AsFloat(q[0]), v0 = AsFloat(q[1]);
    const float u3 = AsFloat(q[21]), v3 = AsFloat(q[22]);
    const float x0 = static_cast<float>(static_cast<int32_t>(q[4]));
    const float y0 = static_cast<float>(static_cast<int32_t>(q[5]));
    const float x3 = static_cast<float>(static_cast<int32_t>(q[25]));
    const float y3 = static_cast<float>(static_cast<int32_t>(q[26]));
    if (x3 == x0 || y3 == y0) return false;
    out->sx = (u3 - u0) / (x3 - x0);
    out->sy = (v3 - v0) / (y3 - y0);
    // Anchor on the corner with the smallest texel coordinates, the order the shader walks in.
    out->u0 = out->sx >= 0 ? u0 : u3;
    out->v0 = out->sy >= 0 ? v0 : v3;
    out->x0 = out->sx >= 0 ? x0 : x3;
    out->y0 = out->sy >= 0 ? y0 : y3;
    out->sx = std::fabs(out->sx);
    out->sy = std::fabs(out->sy);
    return true;
}

}  // namespace

void Init(const AtmtModApi* api, LogFn log, const int32_t* setting, const float* weight) {
    g_api = api;
    g_log = log;
    g_setting = setting;
    g_weight = weight;
    g_d3d11 = GetModuleHandleW(L"d3d11.dll");
    InitializeCriticalSection(&g_lock);
}

// Game thread, from the text draw hook.
void OnTextDraw(const void* texture, const void* quad) {
    if (g_d3d11 == nullptr) return;
    const LONG enabled = g_setting != nullptr && *g_setting != 0;
    auto* object = static_cast<uint8_t*>(const_cast<void*>(texture));
    EnterCriticalSection(&g_lock);
    const float weight = g_weight != nullptr ? *g_weight : 1.0f;
    if (enabled && weight != g_weight_seen) {
        for (int i = 0; i < g_count; ++i) g_entries[i].dirty = g_entries[i].object != nullptr;
        InterlockedExchange(&g_dirty, 1);
        g_weight_seen = weight;
    }
    if (enabled != g_enabled_seen) {
        // Switched: hand the game its own views back (or let the refresh swap the shadows in again).
        for (int i = 0; i < g_count; ++i) {
            Entry& e = g_entries[i];
            if (e.object == nullptr) continue;
            if (e.swapped && ViewOf(e.object) == e.shadow_view) ViewOf(e.object) = e.original;
            e.swapped = false;
            e.dirty = true;
        }
        InterlockedExchange(&g_dirty, 1);
        g_enabled_seen = enabled;
        g_log("text_prefilter: %s (%ld refreshes so far)", enabled ? "on" : "off", g_refreshes);
    }
    Mapping map{};
    if (!enabled || !Readable(object + kViewOffset, sizeof(void*)) || !ReadMapping(quad, &map)) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    const bool usable = map.sx >= kMinShrink && map.sy >= kMinShrink;
    ID3D11ShaderResourceView* view = ViewOf(object);
    for (int i = 0; i < g_count; ++i) {
        Entry& e = g_entries[i];
        if (e.object == object && (e.original == view || e.shadow_view == view)) {
            if (std::memcmp(&e.map, &map, sizeof(map)) != 0) {
                // Drawn at another size or place: rebuild, or give the game its own texture back.
                e.map = map;
                e.usable = usable;
                if (!usable && e.swapped && ViewOf(e.object) == e.shadow_view) {
                    ViewOf(e.object) = e.original;
                    e.swapped = false;
                }
                e.dirty = true;
                InterlockedExchange(&g_dirty, 1);
            }
            LeaveCriticalSection(&g_lock);
            return;
        }
    }
    if (view == nullptr || !IsD3D11(view)) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    if (!g_hooked) {
        g_hooked = true;
        if (!HookContext(view)) {
            g_log("text_prefilter: context hooks failed - crisp text off");
            g_d3d11 = nullptr;
            LeaveCriticalSection(&g_lock);
            return;
        }
    }
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    ID3D11Texture2D* texture2d = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    if (resource == nullptr ||
        FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture2d)))) {
        if (resource) resource->Release();
        LeaveCriticalSection(&g_lock);
        return;
    }
    texture2d->GetDesc(&desc);
    texture2d->Release();
    if (desc.MipLevels != 1 || desc.ArraySize != 1 || desc.Usage != D3D11_USAGE_DYNAMIC) {
        resource->Release();
        LeaveCriticalSection(&g_lock);
        return;
    }
    // A new text texture (or a reused object with a new view): take an entry, evicting round-robin.
    int slot = -1;
    for (int i = 0; i < g_count; ++i) {
        if (g_entries[i].object == object) slot = i;
    }
    if (slot < 0 && g_count < kMaxEntries) slot = g_count++;
    if (slot < 0) slot = g_next_evict++ % kMaxEntries;
    if (g_entries[slot].object != nullptr) Evict(g_entries[slot]);
    view->AddRef();
    Entry fresh{};
    fresh.object = object;
    fresh.original = view;
    fresh.source = resource;
    fresh.map = map;
    fresh.usable = usable;
    fresh.dirty = true;
    g_entries[slot] = fresh;
    InterlockedExchange(&g_dirty, 1);
    LeaveCriticalSection(&g_lock);
}

}  // namespace text_prefilter
