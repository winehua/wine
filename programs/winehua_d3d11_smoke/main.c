/*
 * WineHua DXVK Legacy D3D11 smoke.
 *
 * The test deliberately uses only the D3D11/DXGI public API.  It draws a
 * deterministic four-colour frame through a hardware D3D11 device and a
 * Win32 swapchain, then records the actual modules loaded by the process.
 * The module paths are authoritative: a WineD3D fallback is not a pass.
 */

#define COBJMACROS

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../winehua_smoke_protocol.h"

struct vertex
{
    float x, y, z;
    float r, g, b, a;
    float u, v;
};

struct cube_constants
{
    float transform[16];
};

#define WINEHUA_FEATURE_PROBE_SIZE 64
#define WINEHUA_RGBA_EXPECTED 0xff332211u
#define WINEHUA_RGBA_MARKER 0xdeadbeefu

struct smoke_state
{
    struct winehua_smoke_options smoke;
    ULONGLONG started_ms;
    HWND hwnd;
    IDXGISwapChain *swapchain;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11Texture2D *backbuffer;
    ID3D11Texture2D *stencil_probe_staging;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11Buffer *vertex_buffer;
    ID3D11Buffer *index_buffer;
    ID3D11Buffer *constant_buffer;
    ID3D11Buffer *dynamic_constant_staging;
    ID3D11InputLayout *input_layout;
    ID3D11VertexShader *vertex_shader;
    ID3D11PixelShader *pixel_shader;
    ID3D11RenderTargetView *probe_rtv;
    ID3D11VertexShader *fullscreen_vertex_shader;
    ID3D11PixelShader *bc_probe_pixel_shader;
    ID3D11PixelShader *rgba_sample_pixel_shader;
    ID3D11PixelShader *rgba_load_pixel_shader;
    ID3D11PixelShader *descriptor_identity_pixel_shader;
    ID3D11PixelShader *subresource_pixel_shader;
    ID3D11PixelShader *stencil_overlay_pixel_shader;
    ID3D11ComputeShader *compute_shader;
    ID3D11ComputeShader *rgba_sample_compute_shader;
    ID3D11ComputeShader *rgba_load_compute_shader;
    ID3D11Texture2D *bc_texture;
    ID3D11ShaderResourceView *bc_srv;
    ID3D11SamplerState *bc_sampler;
    ID3D11Texture2D *pattern_texture;
    ID3D11Texture2D *pattern_staging;
    ID3D11ShaderResourceView *pattern_srv;
    ID3D11SamplerState *linear_sampler;
    ID3D11Texture2D *rgba_texture;
    ID3D11ShaderResourceView *rgba_srv;
    ID3D11Texture2D *rgba_updated_texture;
    ID3D11ShaderResourceView *rgba_updated_srv;
    ID3D11Texture2D *rgba_updated_staging;
    ID3D11SamplerState *point_sampler;
    ID3D11Texture2D *descriptor_textures[4];
    ID3D11ShaderResourceView *descriptor_srvs[4];
    ID3D11Texture2D *msaa_texture;
    ID3D11RenderTargetView *msaa_rtv;
    ID3D11Texture2D *probe_texture;
    ID3D11ShaderResourceView *probe_srv;
    ID3D11Texture2D *probe_staging;
    ID3D11Texture2D *compute_texture;
    ID3D11ShaderResourceView *compute_srv;
    ID3D11UnorderedAccessView *compute_uav;
    ID3D11Texture2D *compute_staging;
    ID3D11Texture2D *subresource_texture;
    ID3D11ShaderResourceView *subresource_srv;
    ID3D11BlendState *blend_state;
    ID3D11DepthStencilState *depth_stencil_state;
    ID3D11DepthStencilState *stencil_read_state;
    ID3D11RasterizerState *rasterizer_state;
    ID3D11Query *stencil_query;
    BOOL query_enabled;
    BOOL bc_texture_ready;
    BOOL pattern_texture_ready;
    BOOL texture_update_ready;
    BOOL texture_upload_functional;
    BOOL alpha_blend_ready;
    BOOL stencil_test_ready;
    BOOL cube_geometry_ready;
    BOOL depth_stencil_ready;
    BOOL constant_buffer_ready;
    BOOL dynamic_constant_mode;
    BOOL dynamic_constant_test;
    BOOL dynamic_constant_readback;
    BOOL rasterizer_state_ready;
    BOOL shader_model_5_ready;
    BOOL draw_indexed_instanced_ready;
    BOOL offscreen_render_ready;
    BOOL bc_sampling_submitted;
    BOOL bc_sampling_functional;
    BOOL msaa4x_supported;
    BOOL msaa_resolve_functional;
    BOOL compute_dispatch_ready;
    BOOL compute_uav_submitted;
    BOOL compute_uav_functional;
    BOOL compute_sampled_functional;
    BOOL texture_sampling_functional;
    BOOL rgba_load_ps_functional;
    BOOL rgba_load_cs_functional;
    BOOL rgba_point_ps_functional;
    BOOL rgba_point_cs_functional;
    BOOL rgba_linear_ps_functional;
    BOOL rgba_linear_cs_functional;
    BOOL rgba_updated_upload_functional;
    BOOL rgba_updated_load_ps_functional;
    BOOL rgba_updated_load_cs_functional;
    BOOL rgba_updated_point_ps_functional;
    BOOL rgba_updated_point_cs_functional;
    BOOL descriptor_identity_functional;
    BOOL descriptor_rebind_functional;
    BOOL descriptor_unbound_functional;
    BOOL descriptor_lifetime_functional;
    BOOL subresource_array_functional;
    BOOL subresource_mip_functional;
    BOOL subresource_explicit_lod_functional;
    BOOL subresource_barrier_functional;
    BOOL subresource_functional;
    BOOL stencil_functional;
    BOOL stencil_pixel_functional;
    BOOL stencil_query_pending;
    UINT frame_count;
    UINT present_failure_frame;
    UINT feature_probe_read_bytes;
    UINT feature_probe_gpu_copies;
    UINT rgba_load_ps_value;
    UINT rgba_load_cs_value;
    UINT rgba_point_ps_value;
    UINT rgba_point_cs_value;
    UINT rgba_linear_ps_value;
    UINT rgba_linear_cs_value;
    UINT rgba_updated_upload_value;
    UINT rgba_updated_load_ps_value;
    UINT rgba_updated_load_cs_value;
    UINT rgba_updated_point_ps_value;
    UINT rgba_updated_point_cs_value;
    UINT descriptor_identity_values[4];
    UINT descriptor_rebind_values[4];
    UINT descriptor_unbound_values[4];
    UINT descriptor_lifetime_values[4];
    UINT subresource_initial_values[4];
    UINT subresource_updated_values[4];
    UINT64 stencil_samples;
    HRESULT present_result;
    D3D_FEATURE_LEVEL feature_level;
    char d3d11_module[MAX_PATH];
    char dxgi_module[MAX_PATH];
};

/* The Wine import library does not export the SDK's IID symbol on all
 * architectures.  Keep the public D3D11 texture IID local to the smoke so
 * x86 and x64 link identically. */
static const GUID WINEHUA_IID_ID3D11Texture2D =
    {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

static void module_path(const char *name, char *path, size_t size)
{
    HMODULE module = GetModuleHandleA(name);
    if (module) GetModuleFileNameA(module, path, (DWORD)size);
}

static void safe_json_text(char *output, size_t output_size, const char *input)
{
    size_t written = 0;
    if (!output_size) return;
    while (input && *input && written + 1 < output_size)
    {
        unsigned char ch = (unsigned char)*input++;
        if (ch == '"' || ch == '\\' || ch < 0x20 || ch > 0x7e) ch = '_';
        output[written++] = (char)ch;
    }
    output[written] = 0;
}

static BOOL module_is_native(const char *path)
{
    /* Wine builtin modules are reported as an internal `unix\\...` path,
     * while native PE DLLs retain a Windows drive-letter path such as
     * C:\\smoke\\x86\\d3d11.dll.  Do not require the filename to contain
     * "dxvk": the managed runtime deliberately keeps the standard DLL names
     * so Windows loader selection remains transparent. */
    return path && path[0] && path[1] == ':';
}

static BOOL module_is_managed_dxvk(const char *path)
{
    const char *root = winehua_smoke_env("WINEHUA_DXVK_ROOT", "");
    char normalized_path[1024], normalized_root[512];
    size_t i, length, path_length;

    if (!path || !*path || !root || !*root) return FALSE;
    length = strlen(root);
    if (length >= sizeof(normalized_root)) length = sizeof(normalized_root) - 1;
    for (i = 0; i < length; ++i)
        normalized_root[i] = root[i] == '\\' ? '/' : root[i];
    normalized_root[length] = 0;
    path_length = strlen(path);
    if (path_length >= sizeof(normalized_path)) path_length = sizeof(normalized_path) - 1;
    for (i = 0; i < path_length; ++i)
        normalized_path[i] = path[i] == '\\' ? '/' : path[i];
    normalized_path[path_length] = 0;
    /* GetModuleFileName may return unix/data/.../legacy\x64\... with mixed
     * separators. Normalize both sides and require the selected runtime root,
     * not merely a DLL whose filename happens to contain "dxvk". */
    return strstr(normalized_path, normalized_root) != NULL;
}

static BOOL dxvk_modules_loaded(struct smoke_state *state)
{
    module_path("d3d11.dll", state->d3d11_module, sizeof(state->d3d11_module));
    module_path("dxgi.dll", state->dxgi_module, sizeof(state->dxgi_module));
    return (module_is_native(state->d3d11_module) ||
            module_is_managed_dxvk(state->d3d11_module)) &&
           (module_is_native(state->dxgi_module) ||
            module_is_managed_dxvk(state->dxgi_module));
}

static void write_state(struct smoke_state *state, const char *status,
                        const char *stage, const char *message)
{
    char d3d11_module[MAX_PATH], dxgi_module[MAX_PATH];
    char metrics[8192];
    const char *version = winehua_smoke_env("WINEHUA_DXVK_VERSION", "unknown");
    BOOL dxvk_loaded;

    dxvk_loaded = dxvk_modules_loaded(state);
    safe_json_text(d3d11_module, sizeof(d3d11_module), state->d3d11_module);
    safe_json_text(dxgi_module, sizeof(dxgi_module), state->dxgi_module);
    snprintf(metrics, sizeof(metrics),
             "{\"d3dBackend\":\"dxvk_legacy\",\"dxvkVersion\":\"%s\","
             "\"d3d11Module\":\"%s\",\"dxgiModule\":\"%s\","
             "\"featureLevel\":\"%u.%u\",\"adapter\":\"DXVK Vulkan\","
             "\"vulkanDevice\":\"via winevulkan/Venus\",\"cpuReadBytes\":0,"
             "\"cpuUploadBytes\":0,\"gpuCopyCount\":1,\"queueSubmitCount\":%u,"
             "\"presentFrames\":%u,\"presentFailureFrame\":%u,"
             "\"presentResult\":%ld,\"fallbackDetected\":%s,"
            "\"perFrameDeviceWaitIdle\":0,\"bcTextureTest\":\"%s\","
            "\"bcFormat\":\"BC1_UNORM\",\"bcEmulation\":%s,"
            "\"patternTexture\":\"%s\",\"textureUpdate\":%s,"
            "\"textureUploadReadback\":%s,"
            "\"alphaBlend\":%s,\"stencilTest\":%s,"
            "\"cubeGeometry\":%s,\"depthStencil\":%s,"
            "\"constantBuffer\":%s,\"rasterizerState\":%s,"
            "\"shaderModel\":\"5.0\",\"shaderModel5\":%s,"
            "\"drawIndexedInstanced\":%s,\"instanceCount\":2,"
            "\"offscreenRenderTarget\":%s,\"bcSamplingSubmitted\":%s,\"bcSamplingFunctional\":%s,"
            "\"msaa4xSupported\":%s,\"msaaResolveFunctional\":%s,"
            "\"computeShaderDispatch\":%s,\"computeUavSubmitted\":%s,\"computeUavFunctional\":%s,"
            "\"computeSampledImageFunctional\":%s,"
            "\"stencilQueryEnabled\":%s,\"stencilPixelFunctional\":%s,"
            "\"stencilFunctional\":%s,\"occlusionQuerySamples\":%llu,"
            "\"featureProbeReadBytes\":%u,\"featureProbeGpuCopies\":%u,"
            "\"constantBufferMode\":\"%s\",\"dynamicConstantBuffer\":%s,"
            "\"dynamicConstantReadback\":%s,"
            "\"textureSampling\":%s,"
            "\"rgba8SampleMatrix\":{\"texture\":\"immutable\","
            "\"expectedValue\":\"0x%08x\",\"markerValue\":\"0x%08x\","
            "\"loadPs\":{\"value\":\"0x%08x\",\"pass\":%s},"
            "\"loadCs\":{\"value\":\"0x%08x\",\"pass\":%s},"
            "\"pointPs\":{\"value\":\"0x%08x\",\"pass\":%s},"
            "\"pointCs\":{\"value\":\"0x%08x\",\"pass\":%s},"
            "\"linearPs\":{\"value\":\"0x%08x\",\"pass\":%s},"
            "\"linearCs\":{\"value\":\"0x%08x\",\"pass\":%s},"
            "\"updated\":{\"uploadValue\":\"0x%08x\",\"uploadPass\":%s,"
            "\"loadPs\":{\"value\":\"0x%08x\",\"pass\":%s},"
            "\"loadCs\":{\"value\":\"0x%08x\",\"pass\":%s},"
            "\"pointPs\":{\"value\":\"0x%08x\",\"pass\":%s},"
            "\"pointCs\":{\"value\":\"0x%08x\",\"pass\":%s}}},"
             "\"descriptorMatrix\":{"
            "\"initial\":{\"values\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],\"pass\":%s},"
            "\"rebind\":{\"values\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],\"pass\":%s},"
            "\"unbound\":{\"values\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],\"pass\":%s},"
             "\"lifetime\":{\"values\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],\"pass\":%s}},"
             "\"subresourceMatrix\":{\"initialValues\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],"
             "\"updatedValues\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],"
             "\"arrayLayers\":%s,\"mipLevels\":%s,\"explicitLod\":%s,"
             "\"barrierUpdate\":%s,\"pass\":%s},"
             "\"decodedFormat\":\"R8G8B8A8_UNORM\",\"decodedBytes\":256,"
             "\"cpuDecodeUs\":0,\"durationMs\":%llu}",
             version, d3d11_module, dxgi_module,
             (unsigned int)((state->feature_level >> 12) & 0xf),
             (unsigned int)((state->feature_level >> 0) & 0xf),
             state->frame_count, state->frame_count, state->present_failure_frame,
            (long)state->present_result, dxvk_loaded ? "false" : "true",
            state->bc_texture_ready ? "created_sampled" : "not_created",
            state->bc_texture_ready ? "true" : "false",
            state->pattern_texture_ready ? "created_sampled" : "not_created",
            state->texture_update_ready ? "true" : "false",
            state->texture_upload_functional ? "true" : "false",
            state->alpha_blend_ready ? "true" : "false",
            state->stencil_test_ready ? "true" : "false",
            state->cube_geometry_ready ? "true" : "false",
            state->depth_stencil_ready ? "true" : "false",
            state->constant_buffer_ready ? "true" : "false",
            state->rasterizer_state_ready ? "true" : "false",
            state->shader_model_5_ready ? "true" : "false",
            state->draw_indexed_instanced_ready ? "true" : "false",
            state->offscreen_render_ready ? "true" : "false",
            state->bc_sampling_submitted ? "true" : "false",
            state->bc_sampling_functional ? "true" : "false",
            state->msaa4x_supported ? "true" : "false",
            state->msaa_resolve_functional ? "true" : "false",
            state->compute_dispatch_ready ? "true" : "false",
            state->compute_uav_submitted ? "true" : "false",
            state->compute_uav_functional ? "true" : "false",
            state->compute_sampled_functional ? "true" : "false",
            state->query_enabled ? "true" : "false",
            state->stencil_pixel_functional ? "true" : "false",
            state->stencil_functional ? "true" : "false",
            (unsigned long long)state->stencil_samples,
            state->feature_probe_read_bytes,
            state->feature_probe_gpu_copies,
            state->dynamic_constant_mode ? "dynamic_map" : "immutable_upload",
            state->dynamic_constant_test ? "true" : "false",
            state->dynamic_constant_readback ? "true" : "false",
            state->texture_sampling_functional ? "true" : "false",
            WINEHUA_RGBA_EXPECTED, WINEHUA_RGBA_MARKER,
            state->rgba_load_ps_value,
            state->rgba_load_ps_functional ? "true" : "false",
            state->rgba_load_cs_value,
            state->rgba_load_cs_functional ? "true" : "false",
            state->rgba_point_ps_value,
            state->rgba_point_ps_functional ? "true" : "false",
            state->rgba_point_cs_value,
            state->rgba_point_cs_functional ? "true" : "false",
            state->rgba_linear_ps_value,
            state->rgba_linear_ps_functional ? "true" : "false",
            state->rgba_linear_cs_value,
            state->rgba_linear_cs_functional ? "true" : "false",
            state->rgba_updated_upload_value,
            state->rgba_updated_upload_functional ? "true" : "false",
            state->rgba_updated_load_ps_value,
            state->rgba_updated_load_ps_functional ? "true" : "false",
            state->rgba_updated_load_cs_value,
            state->rgba_updated_load_cs_functional ? "true" : "false",
            state->rgba_updated_point_ps_value,
            state->rgba_updated_point_ps_functional ? "true" : "false",
            state->rgba_updated_point_cs_value,
            state->rgba_updated_point_cs_functional ? "true" : "false",
            state->descriptor_identity_values[0],
            state->descriptor_identity_values[1],
            state->descriptor_identity_values[2],
            state->descriptor_identity_values[3],
            state->descriptor_identity_functional ? "true" : "false",
            state->descriptor_rebind_values[0],
            state->descriptor_rebind_values[1],
            state->descriptor_rebind_values[2],
            state->descriptor_rebind_values[3],
            state->descriptor_rebind_functional ? "true" : "false",
            state->descriptor_unbound_values[0],
            state->descriptor_unbound_values[1],
            state->descriptor_unbound_values[2],
            state->descriptor_unbound_values[3],
            state->descriptor_unbound_functional ? "true" : "false",
            state->descriptor_lifetime_values[0],
             state->descriptor_lifetime_values[1],
             state->descriptor_lifetime_values[2],
             state->descriptor_lifetime_values[3],
             state->descriptor_lifetime_functional ? "true" : "false",
             state->subresource_initial_values[0],
             state->subresource_initial_values[1],
             state->subresource_initial_values[2],
             state->subresource_initial_values[3],
             state->subresource_updated_values[0],
             state->subresource_updated_values[1],
             state->subresource_updated_values[2],
             state->subresource_updated_values[3],
             state->subresource_array_functional ? "true" : "false",
             state->subresource_mip_functional ? "true" : "false",
             state->subresource_explicit_lod_functional ? "true" : "false",
             state->subresource_barrier_functional ? "true" : "false",
             state->subresource_functional ? "true" : "false",
             winehua_smoke_timestamp_ms() - state->started_ms);
    winehua_smoke_write_result(&state->smoke, status, stage, message, metrics);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE)
    {
        DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

static BOOL compile_shader(const char *source, const char *entry, const char *target,
                           ID3DBlob **blob)
{
    ID3DBlob *errors = NULL;
    HRESULT result = D3DCompile(source, strlen(source), "winehua_d3d11_smoke.hlsl",
                                NULL, NULL, entry, target, 0, 0, blob, &errors);
    if (FAILED(result))
    {
        if (errors)
            fprintf(stderr, "winehua_d3d11_smoke: shader compile: %.*s\n",
                    (int)ID3D10Blob_GetBufferSize(errors),
                    (const char *)ID3D10Blob_GetBufferPointer(errors));
        if (errors) ID3D10Blob_Release(errors);
        return FALSE;
    }
    if (errors) ID3D10Blob_Release(errors);
    return TRUE;
}

static BOOL create_bc_texture(struct smoke_state *state)
{
    /* One BC1 block whose first palette entry is opaque red. The texture is
     * deliberately created with the compressed DXGI format so the DXVK
     * upload-time fallback is exercised by the normal D3D11 API. */
    static const unsigned char bc1_blocks[32] = {
        /* top-left red, top-right green */
        0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xe0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* bottom-left blue, bottom-right white */
        0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_SUBRESOURCE_DATA data = {0};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    D3D11_SAMPLER_DESC sampler_desc = {0};
    HRESULT result;

    desc.Width = 8;
    desc.Height = 8;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_BC1_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    data.pSysMem = bc1_blocks;
    data.SysMemPitch = 16;
    data.SysMemSlicePitch = sizeof(bc1_blocks);

    result = ID3D11Device_CreateTexture2D(state->device, &desc, &data,
                                          &state->bc_texture);
    if (FAILED(result)) {
        state->present_result = result;
        fprintf(stderr, "winehua_d3d11_smoke: BC1 texture creation failed: 0x%08lx\n",
                (unsigned long)result);
        return FALSE;
    }

    srv_desc.Format = DXGI_FORMAT_BC1_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    result = ID3D11Device_CreateShaderResourceView(
        state->device, (ID3D11Resource *)state->bc_texture, &srv_desc,
        &state->bc_srv);
    if (FAILED(result)) {
        state->present_result = result;
        fprintf(stderr, "winehua_d3d11_smoke: BC1 SRV creation failed: 0x%08lx\n",
                (unsigned long)result);
        return FALSE;
    }

    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0.0f;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    result = ID3D11Device_CreateSamplerState(state->device, &sampler_desc,
                                               &state->bc_sampler);
    if (FAILED(result)) {
        state->present_result = result;
        fprintf(stderr, "winehua_d3d11_smoke: BC1 sampler creation failed: 0x%08lx\n",
                (unsigned long)result);
        return FALSE;
    }

    state->bc_texture_ready = TRUE;
    return TRUE;
}

static BOOL create_pattern_texture(struct smoke_state *state)
{
    /* A small mutable RGBA texture makes the smoke exercise a normal
     * UpdateSubresource upload and linear filtering, in addition to the BC1
     * path above.  Sampling the four distinct texels also makes a stale or
     * missing upload visible in the captured cube rather than only in JSON. */
    static const unsigned int initial_pixels[4] = {
        0xff2020ff, 0xff20a0ff, 0xff20a020, 0xffe0e020
    };
    static const unsigned int updated_pixels[4] = {
        0xff2040e0, 0xff40e0e0, 0xffe04040, 0xffe0a020
    };
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_SUBRESOURCE_DATA data = {0};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    D3D11_SAMPLER_DESC sampler_desc = {0};
    HRESULT result;

    desc.Width = 2;
    desc.Height = 2;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    data.pSysMem = initial_pixels;
    data.SysMemPitch = sizeof(unsigned int) * 2;
    result = ID3D11Device_CreateTexture2D(state->device, &desc, &data,
                                          &state->pattern_texture);
    if (FAILED(result)) {
        state->present_result = result;
        fprintf(stderr, "winehua_d3d11_smoke: pattern texture creation failed: 0x%08lx\n",
                (unsigned long)result);
        return FALSE;
    }

    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    result = ID3D11Device_CreateShaderResourceView(
        state->device, (ID3D11Resource *)state->pattern_texture, &srv_desc,
        &state->pattern_srv);
    if (FAILED(result)) {
        state->present_result = result;
        return FALSE;
    }

    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0.0f;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    result = ID3D11Device_CreateSamplerState(state->device, &sampler_desc,
                                               &state->linear_sampler);
    if (FAILED(result)) {
        state->present_result = result;
        return FALSE;
    }

    state->pattern_texture_ready = TRUE;
    ID3D11DeviceContext_UpdateSubresource(
        state->context, (ID3D11Resource *)state->pattern_texture, 0, NULL,
        updated_pixels, sizeof(unsigned int) * 2, 0);
    state->texture_update_ready = TRUE;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    result = ID3D11Device_CreateTexture2D(
        state->device, &desc, NULL, &state->pattern_staging);
    if (FAILED(result)) {
        state->present_result = result;
        return FALSE;
    }
    return TRUE;
}

static BOOL create_rgba_sample_texture(struct smoke_state *state)
{
    static const unsigned int pixels[16] = {
        WINEHUA_RGBA_EXPECTED, WINEHUA_RGBA_EXPECTED,
        WINEHUA_RGBA_EXPECTED, WINEHUA_RGBA_EXPECTED,
        WINEHUA_RGBA_EXPECTED, WINEHUA_RGBA_EXPECTED,
        WINEHUA_RGBA_EXPECTED, WINEHUA_RGBA_EXPECTED,
        WINEHUA_RGBA_EXPECTED, WINEHUA_RGBA_EXPECTED,
        WINEHUA_RGBA_EXPECTED, WINEHUA_RGBA_EXPECTED,
        WINEHUA_RGBA_EXPECTED, WINEHUA_RGBA_EXPECTED,
        WINEHUA_RGBA_EXPECTED, WINEHUA_RGBA_EXPECTED,
    };
    static const unsigned int zeros[16] = {0};
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_SUBRESOURCE_DATA data = {0};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    D3D11_SAMPLER_DESC sampler_desc = {0};
    HRESULT result;

    desc.Width = 4;
    desc.Height = 4;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    data.pSysMem = pixels;
    data.SysMemPitch = 4 * sizeof(unsigned int);
    data.SysMemSlicePitch = sizeof(pixels);
    result = ID3D11Device_CreateTexture2D(
        state->device, &desc, &data, &state->rgba_texture);
    if (FAILED(result)) {
        state->present_result = result;
        return FALSE;
    }

    srv_desc.Format = desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    result = ID3D11Device_CreateShaderResourceView(
        state->device, (ID3D11Resource *)state->rgba_texture, &srv_desc,
        &state->rgba_srv);
    if (FAILED(result)) {
        state->present_result = result;
        return FALSE;
    }

    /* A second copy follows the normal D3D11 DEFAULT + UpdateSubresource
     * route. It is intentionally separate from the immutable image so an
     * initial-data upload problem cannot be confused with sampled-image
     * execution. */
    desc.Usage = D3D11_USAGE_DEFAULT;
    data.pSysMem = zeros;
    result = ID3D11Device_CreateTexture2D(
        state->device, &desc, &data, &state->rgba_updated_texture);
    if (FAILED(result)) {
        state->present_result = result;
        return FALSE;
    }
    result = ID3D11Device_CreateShaderResourceView(
        state->device, (ID3D11Resource *)state->rgba_updated_texture,
        &srv_desc, &state->rgba_updated_srv);
    if (FAILED(result)) {
        state->present_result = result;
        return FALSE;
    }
    ID3D11DeviceContext_UpdateSubresource(
        state->context, (ID3D11Resource *)state->rgba_updated_texture, 0,
        NULL, pixels, 4 * sizeof(unsigned int), 0);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    result = ID3D11Device_CreateTexture2D(
        state->device, &desc, NULL, &state->rgba_updated_staging);
    if (FAILED(result)) {
        state->present_result = result;
        return FALSE;
    }

    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0.0f;
    sampler_desc.MaxLOD = 0.0f;
    result = ID3D11Device_CreateSamplerState(
        state->device, &sampler_desc, &state->point_sampler);
    if (FAILED(result)) {
        state->present_result = result;
        return FALSE;
    }
    return TRUE;
}

static BOOL create_solid_rgba_texture(struct smoke_state *state, UINT color,
                                      ID3D11Texture2D **texture,
                                      ID3D11ShaderResourceView **srv)
{
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_SUBRESOURCE_DATA data = {0};
    HRESULT result;

    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    data.pSysMem = &color;
    data.SysMemPitch = sizeof(color);
    result = ID3D11Device_CreateTexture2D(state->device, &desc, &data, texture);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)*texture, NULL, srv);
    if (FAILED(result)) {
        if (*srv) { ID3D11ShaderResourceView_Release(*srv); *srv = NULL; }
        if (*texture) { ID3D11Texture2D_Release(*texture); *texture = NULL; }
        state->present_result = result;
        return FALSE;
    }
    return TRUE;
}

static BOOL create_descriptor_identity_resources(struct smoke_state *state)
{
    static const UINT colors[4] = {
        0xff0000ffu, /* red */
        0xff00ff00u, /* green */
        0xffff0000u, /* blue */
        0xff00ffffu, /* yellow */
    };
    UINT i;

    for (i = 0; i < 4; ++i)
        if (!create_solid_rgba_texture(state, colors[i],
                                       &state->descriptor_textures[i],
                                       &state->descriptor_srvs[i]))
            return FALSE;
    return TRUE;
}

static BOOL create_feature_shaders(struct smoke_state *state)
{
    const char *fullscreen_source =
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "VSOut main(uint vertex_id : SV_VertexID) { VSOut output;"
        "float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);"
        "output.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);"
        "output.uv = uv; return output; }";
    const char *bc_probe_source =
        "Texture2D inputTexture : register(t0);"
        "SamplerState pointSampler : register(s0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "return inputTexture.SampleLevel(pointSampler, input.uv, 0.0); }";
    const char *rgba_sample_source =
        "Texture2D inputTexture : register(t0);"
        "SamplerState inputSampler : register(s0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "return inputTexture.SampleLevel(inputSampler, float2(0.5, 0.5), 0.0); }";
    const char *rgba_load_source =
        "Texture2D inputTexture : register(t0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "return inputTexture.Load(int3(0, 0, 0)); }";
    const char *descriptor_identity_source =
        "Texture2D tex0 : register(t0);"
        "Texture2D tex1 : register(t1);"
        "Texture2D tex2 : register(t2);"
        "Texture2D tex3 : register(t3);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "int x = input.pos.x >= 32.0; int y = input.pos.y >= 32.0;"
        "int slot = x + (y << 1);"
        "if (slot == 0) return tex0.Load(int3(0, 0, 0));"
        "if (slot == 1) return tex1.Load(int3(0, 0, 0));"
        "if (slot == 2) return tex2.Load(int3(0, 0, 0));"
        "return tex3.Load(int3(0, 0, 0)); }";
    const char *subresource_source =
        "Texture2DArray inputTexture : register(t0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "int layer = input.pos.x >= 32.0;"
        "int lod = input.pos.y >= 32.0;"
        "return inputTexture.Load(int4(0, 0, layer, lod)); }";
    const char *stencil_overlay_source =
        "float4 main(float4 position : SV_Position) : SV_Target {"
        "return float4(0.05, 0.65, 0.75, 0.22); }";
    const char *compute_source =
        "Texture2D inputTexture : register(t0);"
        "SamplerState inputSampler : register(s0);"
        "RWTexture2D<float4> outputTexture : register(u0);"
        "[numthreads(8, 8, 1)]"
        "void main(uint3 id : SV_DispatchThreadID) {"
        "if (id.x == 0 && id.y == 0) {"
        "outputTexture[id.xy] = inputTexture.SampleLevel(inputSampler, float2(0.25, 0.25), 0.0);"
        "} else {"
        "uint checker = ((id.x / 8) + (id.y / 8)) & 1;"
        "outputTexture[id.xy] = checker ? float4(1.0, 0.10, 0.80, 1.0)"
        ": float4(0.0, 0.80, 1.0, 1.0); } }";
    const char *rgba_sample_compute_source =
        "Texture2D inputTexture : register(t0);"
        "SamplerState inputSampler : register(s0);"
        "RWTexture2D<float4> outputTexture : register(u0);"
        "[numthreads(8, 8, 1)]"
        "void main(uint3 id : SV_DispatchThreadID) {"
        "if (id.x == 0 && id.y == 0)"
        "outputTexture[id.xy] = inputTexture.SampleLevel(inputSampler, float2(0.5, 0.5), 0.0);"
        "else { uint checker = ((id.x / 8) + (id.y / 8)) & 1;"
        "outputTexture[id.xy] = checker ? float4(1.0, 0.10, 0.80, 1.0)"
        ": float4(0.0, 0.80, 1.0, 1.0); } }";
    const char *rgba_load_compute_source =
        "Texture2D inputTexture : register(t0);"
        "RWTexture2D<float4> outputTexture : register(u0);"
        "[numthreads(8, 8, 1)]"
        "void main(uint3 id : SV_DispatchThreadID) {"
        "if (id.x == 0 && id.y == 0)"
        "outputTexture[id.xy] = inputTexture.Load(int3(0, 0, 0));"
        "else { uint checker = ((id.x / 8) + (id.y / 8)) & 1;"
        "outputTexture[id.xy] = checker ? float4(1.0, 0.10, 0.80, 1.0)"
        ": float4(0.0, 0.80, 1.0, 1.0); } }";
    ID3DBlob *fullscreen_blob = NULL, *probe_blob = NULL;
    ID3DBlob *overlay_blob = NULL, *compute_blob = NULL;
    ID3DBlob *rgba_sample_blob = NULL, *rgba_load_blob = NULL;
    ID3DBlob *descriptor_identity_blob = NULL, *subresource_blob = NULL;
    ID3DBlob *rgba_sample_compute_blob = NULL, *rgba_load_compute_blob = NULL;
    HRESULT result = E_FAIL;

    if (!compile_shader(fullscreen_source, "main", "vs_5_0", &fullscreen_blob) ||
        !compile_shader(bc_probe_source, "main", "ps_5_0", &probe_blob) ||
        !compile_shader(rgba_sample_source, "main", "ps_5_0", &rgba_sample_blob) ||
        !compile_shader(rgba_load_source, "main", "ps_5_0", &rgba_load_blob) ||
        !compile_shader(descriptor_identity_source, "main", "ps_5_0", &descriptor_identity_blob) ||
        !compile_shader(subresource_source, "main", "ps_5_0", &subresource_blob) ||
        !compile_shader(stencil_overlay_source, "main", "ps_5_0", &overlay_blob) ||
        !compile_shader(compute_source, "main", "cs_5_0", &compute_blob) ||
        !compile_shader(rgba_sample_compute_source, "main", "cs_5_0", &rgba_sample_compute_blob) ||
        !compile_shader(rgba_load_compute_source, "main", "cs_5_0", &rgba_load_compute_blob))
        goto done;

    result = ID3D11Device_CreateVertexShader(
        state->device, ID3D10Blob_GetBufferPointer(fullscreen_blob),
        ID3D10Blob_GetBufferSize(fullscreen_blob), NULL,
        &state->fullscreen_vertex_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreatePixelShader(
            state->device, ID3D10Blob_GetBufferPointer(rgba_sample_blob),
            ID3D10Blob_GetBufferSize(rgba_sample_blob), NULL,
            &state->rgba_sample_pixel_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreatePixelShader(
            state->device, ID3D10Blob_GetBufferPointer(rgba_load_blob),
            ID3D10Blob_GetBufferSize(rgba_load_blob), NULL,
            &state->rgba_load_pixel_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreatePixelShader(
            state->device, ID3D10Blob_GetBufferPointer(descriptor_identity_blob),
            ID3D10Blob_GetBufferSize(descriptor_identity_blob), NULL,
            &state->descriptor_identity_pixel_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreatePixelShader(
            state->device, ID3D10Blob_GetBufferPointer(subresource_blob),
            ID3D10Blob_GetBufferSize(subresource_blob), NULL,
            &state->subresource_pixel_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreatePixelShader(
            state->device, ID3D10Blob_GetBufferPointer(probe_blob),
            ID3D10Blob_GetBufferSize(probe_blob), NULL,
            &state->bc_probe_pixel_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreatePixelShader(
            state->device, ID3D10Blob_GetBufferPointer(overlay_blob),
            ID3D10Blob_GetBufferSize(overlay_blob), NULL,
            &state->stencil_overlay_pixel_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateComputeShader(
            state->device, ID3D10Blob_GetBufferPointer(compute_blob),
            ID3D10Blob_GetBufferSize(compute_blob), NULL,
            &state->compute_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateComputeShader(
            state->device, ID3D10Blob_GetBufferPointer(rgba_sample_compute_blob),
            ID3D10Blob_GetBufferSize(rgba_sample_compute_blob), NULL,
            &state->rgba_sample_compute_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateComputeShader(
            state->device, ID3D10Blob_GetBufferPointer(rgba_load_compute_blob),
            ID3D10Blob_GetBufferSize(rgba_load_compute_blob), NULL,
            &state->rgba_load_compute_shader);

done:
    if (fullscreen_blob) ID3D10Blob_Release(fullscreen_blob);
    if (probe_blob) ID3D10Blob_Release(probe_blob);
    if (overlay_blob) ID3D10Blob_Release(overlay_blob);
    if (compute_blob) ID3D10Blob_Release(compute_blob);
    if (rgba_sample_blob) ID3D10Blob_Release(rgba_sample_blob);
    if (rgba_load_blob) ID3D10Blob_Release(rgba_load_blob);
    if (descriptor_identity_blob) ID3D10Blob_Release(descriptor_identity_blob);
    if (subresource_blob) ID3D10Blob_Release(subresource_blob);
    if (rgba_sample_compute_blob) ID3D10Blob_Release(rgba_sample_compute_blob);
    if (rgba_load_compute_blob) ID3D10Blob_Release(rgba_load_compute_blob);
    if (FAILED(result))
    {
        state->present_result = result;
        return FALSE;
    }
    return TRUE;
}

static BOOL create_feature_resources(struct smoke_state *state)
{
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_QUERY_DESC query_desc = {0};
    UINT quality_levels = 0;
    HRESULT result;

    result = ID3D11Device_CheckMultisampleQualityLevels(
        state->device, DXGI_FORMAT_R8G8B8A8_UNORM, 4, &quality_levels);
    if (FAILED(result) || !quality_levels)
    {
        state->present_result = FAILED(result) ? result : E_FAIL;
        return FALSE;
    }
    state->msaa4x_supported = TRUE;

    desc.Width = WINEHUA_FEATURE_PROBE_SIZE;
    desc.Height = WINEHUA_FEATURE_PROBE_SIZE;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 4;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    result = ID3D11Device_CreateTexture2D(state->device, &desc, NULL,
                                           &state->msaa_texture);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateRenderTargetView(
            state->device, (ID3D11Resource *)state->msaa_texture, NULL,
            &state->msaa_rtv);

    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateTexture2D(state->device, &desc, NULL,
                                               &state->probe_texture);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)state->probe_texture, NULL,
            &state->probe_srv);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateRenderTargetView(
            state->device, (ID3D11Resource *)state->probe_texture, NULL,
            &state->probe_rtv);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateTexture2D(state->device, &desc, NULL,
                                               &state->probe_staging);

    /* Keep subresource coverage independent from the ordinary 2D texture
     * probes.  The four initial subresources intentionally carry different
     * colors: layer 0/mip 0 = red, layer 1/mip 0 = green, layer 0/mip 1 =
     * blue, and layer 1/mip 1 = yellow.  The shader below selects both the
     * array layer and an explicit LOD from the output quadrant, so a stale
     * descriptor, wrong subresource index, or ignored LOD is visible in one
     * deterministic readback. */
    if (SUCCEEDED(result))
    {
        D3D11_TEXTURE2D_DESC sub_desc = {0};
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
        D3D11_SUBRESOURCE_DATA init_data[4] = {0};
        UINT mip0_layer0[8 * 8], mip0_layer1[8 * 8];
        UINT mip1_layer0[4 * 4], mip1_layer1[4 * 4];
        const UINT colors[4] = {
            0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff00ffffu
        };
        UINT i;

        for (i = 0; i < sizeof(mip0_layer0) / sizeof(mip0_layer0[0]); ++i)
        {
            mip0_layer0[i] = colors[0];
            mip0_layer1[i] = colors[1];
        }
        for (i = 0; i < sizeof(mip1_layer0) / sizeof(mip1_layer0[0]); ++i)
        {
            mip1_layer0[i] = colors[2];
            mip1_layer1[i] = colors[3];
        }

        sub_desc.Width = 8;
        sub_desc.Height = 8;
        sub_desc.MipLevels = 2;
        sub_desc.ArraySize = 2;
        sub_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sub_desc.SampleDesc.Count = 1;
        sub_desc.Usage = D3D11_USAGE_DEFAULT;
        sub_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        /* Keep this arithmetic local instead of calling the helper from the
         * D3D11 SDK.  Some Wine import headers declare D3D11CalcSubresource
         * as an external symbol, which is not part of the D3D11 runtime ABI. */
        init_data[0].pSysMem = mip0_layer0;
        init_data[0].SysMemPitch = 8 * sizeof(UINT);
        init_data[0].SysMemSlicePitch = sizeof(mip0_layer0);
        init_data[2].pSysMem = mip0_layer1;
        init_data[2].SysMemPitch = 8 * sizeof(UINT);
        init_data[2].SysMemSlicePitch = sizeof(mip0_layer1);
        init_data[1].pSysMem = mip1_layer0;
        init_data[1].SysMemPitch = 4 * sizeof(UINT);
        init_data[1].SysMemSlicePitch = sizeof(mip1_layer0);
        init_data[3].pSysMem = mip1_layer1;
        init_data[3].SysMemPitch = 4 * sizeof(UINT);
        init_data[3].SysMemSlicePitch = sizeof(mip1_layer1);

        result = ID3D11Device_CreateTexture2D(
            state->device, &sub_desc, init_data, &state->subresource_texture);
        if (SUCCEEDED(result))
        {
            srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srv_desc.Texture2DArray.MostDetailedMip = 0;
            srv_desc.Texture2DArray.MipLevels = 2;
            srv_desc.Texture2DArray.FirstArraySlice = 0;
            srv_desc.Texture2DArray.ArraySize = 2;
            result = ID3D11Device_CreateShaderResourceView(
                state->device, (ID3D11Resource *)state->subresource_texture,
                &srv_desc, &state->subresource_srv);
        }
    }

    desc.Width = 16;
    desc.Height = 16;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.CPUAccessFlags = 0;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateTexture2D(state->device, &desc, NULL,
                                               &state->compute_texture);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)state->compute_texture, NULL,
            &state->compute_srv);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateUnorderedAccessView(
            state->device, (ID3D11Resource *)state->compute_texture, NULL,
            &state->compute_uav);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateTexture2D(state->device, &desc, NULL,
                                               &state->compute_staging);

    if (state->query_enabled)
    {
        query_desc.Query = D3D11_QUERY_OCCLUSION;
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateQuery(state->device, &query_desc,
                                               &state->stencil_query);
    }
    if (FAILED(result))
    {
        state->present_result = result;
        return FALSE;
    }
    return TRUE;
}

static BOOL check_probe_pixels(const D3D11_MAPPED_SUBRESOURCE *mapped)
{
    const BYTE *red = (const BYTE *)mapped->pData + 16 * mapped->RowPitch + 16 * 4;
    const BYTE *green = (const BYTE *)mapped->pData + 16 * mapped->RowPitch + 48 * 4;
    const BYTE *blue = (const BYTE *)mapped->pData + 48 * mapped->RowPitch + 16 * 4;
    const BYTE *white = (const BYTE *)mapped->pData + 48 * mapped->RowPitch + 48 * 4;
    BOOL pass = red[0] > 200 && red[1] < 70 && red[2] < 70 &&
                green[0] < 70 && green[1] > 200 && green[2] < 70 &&
                blue[0] < 70 && blue[1] < 70 && blue[2] > 200 &&
                white[0] > 200 && white[1] > 200 && white[2] > 200;
    fprintf(stderr, "winehua_d3d11_smoke: probe pixels "
                    "R=%u,%u,%u G=%u,%u,%u B=%u,%u,%u W=%u,%u,%u\\n",
            red[0], red[1], red[2], green[0], green[1], green[2],
            blue[0], blue[1], blue[2], white[0], white[1], white[2]);
    return pass;
}

static BOOL check_compute_pixels(const D3D11_MAPPED_SUBRESOURCE *mapped,
                                 BOOL *sampled_functional)
{
    const BYTE *sampled = (const BYTE *)mapped->pData;
    const BYTE *cyan = (const BYTE *)mapped->pData + 2 * mapped->RowPitch + 2 * 4;
    const BYTE *magenta = (const BYTE *)mapped->pData + 2 * mapped->RowPitch + 10 * 4;
    const BOOL sampled_ok = sampled[0] > 100;
    const BOOL uav_ok = cyan[0] < 40 && cyan[1] > 170 && cyan[2] > 220 &&
                        magenta[0] > 220 && magenta[1] < 70 && magenta[2] > 170;
    fprintf(stderr, "winehua_d3d11_smoke: compute sampled pixel=%u,%u,%u,%u\\n",
            sampled[0], sampled[1], sampled[2], sampled[3]);
    *sampled_functional = sampled_ok;
    return uav_ok;
}

static BOOL check_pattern_pixels(const D3D11_MAPPED_SUBRESOURCE *mapped)
{
    const BYTE *p0 = (const BYTE *)mapped->pData + 16 * mapped->RowPitch + 16 * 4;
    const BYTE *p1 = (const BYTE *)mapped->pData + 16 * mapped->RowPitch + 48 * 4;
    const BYTE *p2 = (const BYTE *)mapped->pData + 48 * mapped->RowPitch + 16 * 4;
    const BYTE *p3 = (const BYTE *)mapped->pData + 48 * mapped->RowPitch + 48 * 4;
    const BOOL nonzero = (p0[0] + p0[1] + p0[2] > 100) &&
                         (p1[0] + p1[1] + p1[2] > 100) &&
                         (p2[0] + p2[1] + p2[2] > 100) &&
                         (p3[0] + p3[1] + p3[2] > 100);
    const BOOL varied = p0[0] != p1[0] || p0[1] != p1[1] ||
                        p0[2] != p1[2] || p2[0] != p3[0] ||
                        p2[1] != p3[1] || p2[2] != p3[2];
    fprintf(stderr, "winehua_d3d11_smoke: pattern pixels "
                    "P0=%u,%u,%u P1=%u,%u,%u P2=%u,%u,%u P3=%u,%u,%u\\n",
            p0[0], p0[1], p0[2], p1[0], p1[1], p1[2],
            p2[0], p2[1], p2[2], p3[0], p3[1], p3[2]);
    return nonzero && varied;
}

static BOOL check_pattern_upload(const D3D11_MAPPED_SUBRESOURCE *mapped)
{
    const BYTE *p0 = (const BYTE *)mapped->pData;
    const BYTE *p1 = p0 + 4;
    const BYTE *p2 = p0 + mapped->RowPitch;
    const BYTE *p3 = p2 + 4;
    const BOOL nonzero = (p0[0] + p0[1] + p0[2] > 100) &&
                         (p1[0] + p1[1] + p1[2] > 100) &&
                         (p2[0] + p2[1] + p2[2] > 100) &&
                         (p3[0] + p3[1] + p3[2] > 100);
    const BOOL varied = p0[0] != p1[0] || p0[1] != p1[1] ||
                        p0[2] != p1[2] || p2[0] != p3[0] ||
                        p2[1] != p3[1] || p2[2] != p3[2];
    fprintf(stderr, "winehua_d3d11_smoke: pattern upload "
                    "P0=%u,%u,%u P1=%u,%u,%u P2=%u,%u,%u P3=%u,%u,%u\\n",
            p0[0], p0[1], p0[2], p1[0], p1[1], p1[2],
            p2[0], p2[1], p2[2], p3[0], p3[1], p3[2]);
    return nonzero && varied;
}

static BOOL check_msaa_resolve_pixels(const D3D11_MAPPED_SUBRESOURCE *mapped)
{
    /* The overlay shader writes a deterministic teal color into every sample
     * of the 4x render target. ResolveSubresource must preserve it in the
     * single-sample texture before the staging readback. Keep a generous
     * UNORM tolerance for format conversion, but reject a clear/black frame. */
    const BYTE *pixel = (const BYTE *)mapped->pData
                      + 32 * mapped->RowPitch + 32 * 4;
    const BOOL pass = pixel[0] < 60 && pixel[1] > 120 && pixel[2] > 150;
    fprintf(stderr, "winehua_d3d11_smoke: MSAA resolve pixel=%u,%u,%u,%u\\n",
            pixel[0], pixel[1], pixel[2], pixel[3]);
    return pass;
}

static UINT rgba8_value(const D3D11_MAPPED_SUBRESOURCE *mapped, UINT x, UINT y)
{
    const BYTE *pixel = (const BYTE *)mapped->pData + y * mapped->RowPitch + x * 4;
    return (UINT)pixel[0] | ((UINT)pixel[1] << 8) |
           ((UINT)pixel[2] << 16) | ((UINT)pixel[3] << 24);
}

static BOOL run_rgba_ps_probe(struct smoke_state *state,
                              ID3D11ShaderResourceView *resource,
                              ID3D11PixelShader *shader,
                              ID3D11SamplerState *sampler,
                              const char *name, UINT *sampled_value)
{
    static const float marker[4] = {
        239.0f / 255.0f, 190.0f / 255.0f,
        173.0f / 255.0f, 222.0f / 255.0f,
    };
    D3D11_VIEWPORT viewport = {0, 0, WINEHUA_FEATURE_PROBE_SIZE,
                               WINEHUA_FEATURE_PROBE_SIZE, 0.0f, 1.0f};
    ID3D11RenderTargetView *targets[] = {state->probe_rtv};
    ID3D11ShaderResourceView *resources[] = {resource};
    ID3D11ShaderResourceView *null_resources[] = {NULL};
    ID3D11SamplerState *samplers[] = {sampler};
    ID3D11SamplerState *null_samplers[] = {NULL};
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT result;

    *sampled_value = WINEHUA_RGBA_MARKER;
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 1, targets, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(state->context, state->probe_rtv, marker);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(state->context, shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(state->context, 0, 1, resources);
    ID3D11DeviceContext_PSSetSamplers(state->context, 0, 1, samplers);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(
        state->context, 0, 1, null_resources);
    ID3D11DeviceContext_PSSetSamplers(state->context, 0, 1, null_samplers);
    ID3D11DeviceContext_PSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);
    ID3D11DeviceContext_CopyResource(
        state->context, (ID3D11Resource *)state->probe_staging,
        (ID3D11Resource *)state->probe_texture);
    state->feature_probe_gpu_copies++;
    ID3D11DeviceContext_Flush(state->context);
    result = ID3D11DeviceContext_Map(
        state->context, (ID3D11Resource *)state->probe_staging, 0,
        D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        fprintf(stderr, "winehua_d3d11_smoke: RGBA %s PS map failed=0x%08lx\\n",
                name, (unsigned long)result);
        return FALSE;
    }
    *sampled_value = rgba8_value(&mapped, 32, 32);
    ID3D11DeviceContext_Unmap(
        state->context, (ID3D11Resource *)state->probe_staging, 0);
    state->feature_probe_read_bytes += WINEHUA_FEATURE_PROBE_SIZE *
                                       WINEHUA_FEATURE_PROBE_SIZE * 4;
    fprintf(stderr, "winehua_d3d11_smoke: RGBA %s PS value=0x%08x expected=0x%08x\\n",
            name, *sampled_value, WINEHUA_RGBA_EXPECTED);
    return *sampled_value == WINEHUA_RGBA_EXPECTED;
}

static BOOL run_rgba_cs_probe(struct smoke_state *state,
                              ID3D11ShaderResourceView *resource,
                              ID3D11ComputeShader *shader,
                              ID3D11SamplerState *sampler,
                              const char *name, UINT *sampled_value)
{
    static const float marker[4] = {
        239.0f / 255.0f, 190.0f / 255.0f,
        173.0f / 255.0f, 222.0f / 255.0f,
    };
    ID3D11ShaderResourceView *resources[] = {resource};
    ID3D11ShaderResourceView *null_resources[] = {NULL};
    ID3D11SamplerState *samplers[] = {sampler};
    ID3D11SamplerState *null_samplers[] = {NULL};
    ID3D11UnorderedAccessView *uavs[] = {state->compute_uav};
    ID3D11UnorderedAccessView *null_uavs[] = {NULL};
    D3D11_MAPPED_SUBRESOURCE mapped;
    const BYTE *cyan, *magenta;
    BOOL shader_executed;
    HRESULT result;

    *sampled_value = WINEHUA_RGBA_MARKER;
    ID3D11DeviceContext_ClearUnorderedAccessViewFloat(
        state->context, state->compute_uav, marker);
    ID3D11DeviceContext_CSSetShader(state->context, shader, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(state->context, 0, 1, resources);
    ID3D11DeviceContext_CSSetSamplers(state->context, 0, 1, samplers);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(
        state->context, 0, 1, uavs, NULL);
    ID3D11DeviceContext_Dispatch(state->context, 2, 2, 1);
    ID3D11DeviceContext_CSSetShaderResources(
        state->context, 0, 1, null_resources);
    ID3D11DeviceContext_CSSetSamplers(state->context, 0, 1, null_samplers);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(
        state->context, 0, 1, null_uavs, NULL);
    ID3D11DeviceContext_CSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_CopyResource(
        state->context, (ID3D11Resource *)state->compute_staging,
        (ID3D11Resource *)state->compute_texture);
    state->feature_probe_gpu_copies++;
    ID3D11DeviceContext_Flush(state->context);
    result = ID3D11DeviceContext_Map(
        state->context, (ID3D11Resource *)state->compute_staging, 0,
        D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        fprintf(stderr, "winehua_d3d11_smoke: RGBA %s CS map failed=0x%08lx\\n",
                name, (unsigned long)result);
        return FALSE;
    }
    *sampled_value = rgba8_value(&mapped, 0, 0);
    cyan = (const BYTE *)mapped.pData + 2 * mapped.RowPitch + 2 * 4;
    magenta = (const BYTE *)mapped.pData + 2 * mapped.RowPitch + 10 * 4;
    shader_executed = cyan[0] < 40 && cyan[1] > 170 && cyan[2] > 220 &&
                      magenta[0] > 220 && magenta[1] < 70 && magenta[2] > 170;
    ID3D11DeviceContext_Unmap(
        state->context, (ID3D11Resource *)state->compute_staging, 0);
    state->feature_probe_read_bytes += 16 * 16 * 4;
    fprintf(stderr, "winehua_d3d11_smoke: RGBA %s CS value=0x%08x expected=0x%08x shaderExecuted=%u\\n",
            name, *sampled_value, WINEHUA_RGBA_EXPECTED, shader_executed);
    return shader_executed && *sampled_value == WINEHUA_RGBA_EXPECTED;
}

static BOOL run_descriptor_identity_pass(
    struct smoke_state *state,
    ID3D11ShaderResourceView *const *resources,
    const UINT *expected,
    UINT *values,
    const char *name,
    ID3D11Texture2D *release_texture,
    ID3D11ShaderResourceView *release_srv)
{
    static const float marker[4] = {
        239.0f / 255.0f, 190.0f / 255.0f,
        173.0f / 255.0f, 222.0f / 255.0f,
    };
    D3D11_VIEWPORT viewport = {0, 0, WINEHUA_FEATURE_PROBE_SIZE,
                               WINEHUA_FEATURE_PROBE_SIZE, 0.0f, 1.0f};
    ID3D11RenderTargetView *targets[] = {state->probe_rtv};
    ID3D11ShaderResourceView *null_resources[] = {NULL, NULL, NULL, NULL};
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT result;
    UINT x[4] = {16, 48, 16, 48};
    UINT y[4] = {16, 16, 48, 48};
    BOOL pass = TRUE;
    UINT i;

    ID3D11DeviceContext_OMSetRenderTargets(state->context, 1, targets, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(state->context, state->probe_rtv, marker);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(
        state->context, state->descriptor_identity_pixel_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(state->context, 0, 4, resources);

    /* Drop the application's final references after binding.  The immediate
     * context must retain the SRV/resource through the draw and descriptor
     * update, which makes this a real lifetime test rather than a non-NULL
     * handle check. */
    if (release_srv) ID3D11ShaderResourceView_Release(release_srv);
    if (release_texture) ID3D11Texture2D_Release(release_texture);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(
        state->context, 0, 4, null_resources);
    ID3D11DeviceContext_PSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);
    ID3D11DeviceContext_CopyResource(
        state->context, (ID3D11Resource *)state->probe_staging,
        (ID3D11Resource *)state->probe_texture);
    state->feature_probe_gpu_copies++;
    ID3D11DeviceContext_Flush(state->context);
    result = ID3D11DeviceContext_Map(
        state->context, (ID3D11Resource *)state->probe_staging, 0,
        D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        fprintf(stderr, "winehua_d3d11_smoke: descriptor %s map failed=0x%08lx\n",
                name, (unsigned long)result);
        return FALSE;
    }
    for (i = 0; i < 4; ++i) {
        values[i] = rgba8_value(&mapped, x[i], y[i]);
        if (values[i] != expected[i]) pass = FALSE;
    }
    ID3D11DeviceContext_Unmap(
        state->context, (ID3D11Resource *)state->probe_staging, 0);
    state->feature_probe_read_bytes += WINEHUA_FEATURE_PROBE_SIZE *
                                       WINEHUA_FEATURE_PROBE_SIZE * 4;
    fprintf(stderr,
            "winehua_d3d11_smoke: descriptor %s values="
            "0x%08x,0x%08x,0x%08x,0x%08x expected="
            "0x%08x,0x%08x,0x%08x,0x%08x pass=%u\n",
            name, values[0], values[1], values[2], values[3],
            expected[0], expected[1], expected[2], expected[3], pass);
    return pass;
}

static BOOL run_descriptor_identity_probes(struct smoke_state *state)
{
    static const UINT colors[4] = {
        0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff00ffffu,
    };
    static const UINT unbound_expected[4] = {
        0xff0000ffu, 0x00000000u, 0xffff0000u, 0xff00ffffu,
    };
    ID3D11ShaderResourceView *initial[4];
    ID3D11ShaderResourceView *rebind[4];
    ID3D11ShaderResourceView *unbound[4];
    ID3D11ShaderResourceView *lifetime[4];
    ID3D11Texture2D *lifetime_texture = NULL;
    ID3D11ShaderResourceView *lifetime_srv = NULL;
    static const UINT lifetime_expected[4] = {
        0xff0000ffu, 0xff00ff00u, 0xffff00ffu, 0xff00ffffu,
    };

    memcpy(initial, state->descriptor_srvs, sizeof(initial));
    rebind[0] = state->descriptor_srvs[3];
    rebind[1] = state->descriptor_srvs[2];
    rebind[2] = state->descriptor_srvs[1];
    rebind[3] = state->descriptor_srvs[0];
    unbound[0] = state->descriptor_srvs[0];
    unbound[1] = NULL;
    unbound[2] = state->descriptor_srvs[2];
    unbound[3] = state->descriptor_srvs[3];

    state->descriptor_identity_functional = run_descriptor_identity_pass(
        state, initial, colors, state->descriptor_identity_values,
        "initial", NULL, NULL);
    state->descriptor_rebind_functional = run_descriptor_identity_pass(
        state, rebind, rebind[0] == state->descriptor_srvs[3] ?
        (const UINT[]){0xff00ffffu, 0xffff0000u, 0xff00ff00u, 0xff0000ffu} : colors,
        state->descriptor_rebind_values, "rebind", NULL, NULL);
    state->descriptor_unbound_functional = run_descriptor_identity_pass(
        state, unbound, unbound_expected, state->descriptor_unbound_values,
        "unbound", NULL, NULL);

    if (!create_solid_rgba_texture(state, 0xffff00ffu,
                                   &lifetime_texture, &lifetime_srv))
        return FALSE;
    lifetime[0] = state->descriptor_srvs[0];
    lifetime[1] = state->descriptor_srvs[1];
    lifetime[2] = lifetime_srv;
    lifetime[3] = state->descriptor_srvs[3];
    state->descriptor_lifetime_functional = run_descriptor_identity_pass(
        state, lifetime, lifetime_expected, state->descriptor_lifetime_values,
        "lifetime", lifetime_texture, lifetime_srv);
    return state->descriptor_identity_functional &&
           state->descriptor_rebind_functional &&
           state->descriptor_unbound_functional &&
           state->descriptor_lifetime_functional;
}

static BOOL run_subresource_pass(struct smoke_state *state,
                                 const UINT *expected, UINT *values,
                                 const char *name)
{
    static const float marker[4] = {
        239.0f / 255.0f, 190.0f / 255.0f,
        173.0f / 255.0f, 222.0f / 255.0f,
    };
    static const UINT x[4] = {16, 48, 16, 48};
    static const UINT y[4] = {16, 16, 48, 48};
    D3D11_VIEWPORT viewport = {0, 0, WINEHUA_FEATURE_PROBE_SIZE,
                               WINEHUA_FEATURE_PROBE_SIZE, 0.0f, 1.0f};
    ID3D11RenderTargetView *targets[] = {state->probe_rtv};
    ID3D11ShaderResourceView *resources[] = {state->subresource_srv};
    ID3D11ShaderResourceView *null_resources[] = {NULL};
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT result;
    BOOL pass = TRUE;
    UINT i;

    ID3D11DeviceContext_OMSetRenderTargets(state->context, 1, targets, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(state->context, state->probe_rtv,
                                               marker);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(
        state->context, state->subresource_pixel_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(
        state->context, 0, 1, resources);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(
        state->context, 0, 1, null_resources);
    ID3D11DeviceContext_PSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);
    ID3D11DeviceContext_CopyResource(
        state->context, (ID3D11Resource *)state->probe_staging,
        (ID3D11Resource *)state->probe_texture);
    state->feature_probe_gpu_copies++;
    ID3D11DeviceContext_Flush(state->context);

    result = ID3D11DeviceContext_Map(
        state->context, (ID3D11Resource *)state->probe_staging, 0,
        D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        fprintf(stderr, "winehua_d3d11_smoke: subresource %s map failed=0x%08lx\n",
                name, (unsigned long)result);
        return FALSE;
    }

    for (i = 0; i < 4; ++i)
    {
        values[i] = rgba8_value(&mapped, x[i], y[i]);
        if (values[i] != expected[i]) pass = FALSE;
    }
    ID3D11DeviceContext_Unmap(
        state->context, (ID3D11Resource *)state->probe_staging, 0);
    state->feature_probe_read_bytes += WINEHUA_FEATURE_PROBE_SIZE *
                                       WINEHUA_FEATURE_PROBE_SIZE * 4;
    fprintf(stderr,
            "winehua_d3d11_smoke: subresource %s values="
            "0x%08x,0x%08x,0x%08x,0x%08x expected="
            "0x%08x,0x%08x,0x%08x,0x%08x pass=%u\n",
            name, values[0], values[1], values[2], values[3],
            expected[0], expected[1], expected[2], expected[3], pass);
    return pass;
}

static BOOL run_subresource_probes(struct smoke_state *state)
{
    static const UINT initial_expected[4] = {
        0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff00ffffu,
    };
    static const UINT updated_expected[4] = {
        0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffff00ffu,
    };
    UINT updated_mip1_layer1[4 * 4];
    BOOL initial_pass, updated_pass;
    UINT i;

    initial_pass = run_subresource_pass(
        state, initial_expected, state->subresource_initial_values, "initial");

    /* The first pass has completed through a staging map.  Update only
     * layer=1/mip=1 and sample it again.  This forces the D3D11 runtime/DXVK
     * state tracker through sampled -> transfer/update -> sampled without
     * changing the other three subresources. */
    for (i = 0; i < sizeof(updated_mip1_layer1) / sizeof(updated_mip1_layer1[0]); ++i)
        updated_mip1_layer1[i] = 0xffff00ffu;
    ID3D11DeviceContext_UpdateSubresource(
        state->context, (ID3D11Resource *)state->subresource_texture,
        1 + 1 * 2, NULL, updated_mip1_layer1,
        4 * sizeof(UINT), 0);
    state->feature_probe_gpu_copies++;

    updated_pass = run_subresource_pass(
        state, updated_expected, state->subresource_updated_values, "updated");

    state->subresource_array_functional = initial_pass &&
        state->subresource_initial_values[0] == initial_expected[0] &&
        state->subresource_initial_values[1] == initial_expected[1] &&
        state->subresource_updated_values[0] == updated_expected[0] &&
        state->subresource_updated_values[1] == updated_expected[1];
    state->subresource_mip_functional = initial_pass &&
        state->subresource_initial_values[2] == initial_expected[2] &&
        state->subresource_initial_values[3] == initial_expected[3] &&
        state->subresource_updated_values[2] == updated_expected[2] &&
        state->subresource_updated_values[3] == updated_expected[3];
    state->subresource_explicit_lod_functional =
        state->subresource_initial_values[2] == initial_expected[2] &&
        state->subresource_initial_values[3] == initial_expected[3] &&
        state->subresource_updated_values[2] == updated_expected[2] &&
        state->subresource_updated_values[3] == updated_expected[3];
    state->subresource_barrier_functional = updated_pass &&
        state->subresource_updated_values[0] == updated_expected[0] &&
        state->subresource_updated_values[1] == updated_expected[1] &&
        state->subresource_updated_values[2] == updated_expected[2] &&
        state->subresource_updated_values[3] == updated_expected[3];
    state->subresource_functional = initial_pass && updated_pass &&
        state->subresource_array_functional && state->subresource_mip_functional &&
        state->subresource_explicit_lod_functional &&
        state->subresource_barrier_functional;
    return state->subresource_functional;
}

static BOOL run_feature_probes(struct smoke_state *state)
{
    D3D11_VIEWPORT viewport = {0, 0, WINEHUA_FEATURE_PROBE_SIZE,
                               WINEHUA_FEATURE_PROBE_SIZE, 0.0f, 1.0f};
    ID3D11RenderTargetView *targets[] = {state->probe_rtv};
    ID3D11ShaderResourceView *bc_resources[] = {state->bc_srv};
    ID3D11ShaderResourceView *null_resources[] = {NULL};
    ID3D11SamplerState *samplers[] = {state->bc_sampler};
    ID3D11ShaderResourceView *pattern_resources[] = {state->pattern_srv};
    ID3D11SamplerState *pattern_samplers[] = {state->linear_sampler};
    ID3D11ShaderResourceView *null_srvs[] = {NULL};
    ID3D11SamplerState *null_samplers[] = {NULL};
    ID3D11UnorderedAccessView *uavs[] = {state->compute_uav};
    ID3D11UnorderedAccessView *null_uavs[] = {NULL};
    D3D11_MAPPED_SUBRESOURCE mapped;
    BOOL probe_readback_ok = FALSE;
    BOOL compute_readback_ok = FALSE;
    HRESULT result;

    /* First verify that the normal RGBA UpdateSubresource reached the image
     * at all. This 2x2 staging copy is deliberately tiny and is separate from
     * the shader-sampling check below, so an upload/transfer failure cannot be
     * confused with a descriptor or sampler failure. */
    ID3D11DeviceContext_CopyResource(
        state->context, (ID3D11Resource *)state->pattern_staging,
        (ID3D11Resource *)state->pattern_texture);
    state->feature_probe_gpu_copies++;
    ID3D11DeviceContext_Flush(state->context);
    result = ID3D11DeviceContext_Map(
        state->context, (ID3D11Resource *)state->pattern_staging, 0,
        D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(result))
    {
        state->texture_upload_functional = check_pattern_upload(&mapped);
        ID3D11DeviceContext_Unmap(
            state->context, (ID3D11Resource *)state->pattern_staging, 0);
        state->feature_probe_read_bytes += 2 * 2 * 4;
    }
    else
        fprintf(stderr, "winehua_d3d11_smoke: pattern upload readback unavailable=0x%08lx\\n",
                (unsigned long)result);

    /* Keep this matrix deliberately smaller than the full renderer: one
     * immutable 4x4 RGBA8 texture, mip 0, fixed coordinates and exact
     * readback. Load removes the sampler, POINT matches the passing raw
     * Vulkan probe, and LINEAR tests filtering without changing any other
     * image parameter. PS and CS results stay independent so a shader-stage
     * specific Maleoon/Venus/DXVK failure cannot be hidden. */
    state->rgba_load_ps_functional = run_rgba_ps_probe(
        state, state->rgba_srv, state->rgba_load_pixel_shader, NULL, "Load",
        &state->rgba_load_ps_value);
    state->rgba_load_cs_functional = run_rgba_cs_probe(
        state, state->rgba_srv, state->rgba_load_compute_shader, NULL, "Load",
        &state->rgba_load_cs_value);
    state->rgba_point_ps_functional = run_rgba_ps_probe(
        state, state->rgba_srv, state->rgba_sample_pixel_shader,
        state->point_sampler, "POINT",
        &state->rgba_point_ps_value);
    state->rgba_point_cs_functional = run_rgba_cs_probe(
        state, state->rgba_srv, state->rgba_sample_compute_shader,
        state->point_sampler, "POINT",
        &state->rgba_point_cs_value);
    state->rgba_linear_ps_functional = run_rgba_ps_probe(
        state, state->rgba_srv, state->rgba_sample_pixel_shader,
        state->linear_sampler, "LINEAR",
        &state->rgba_linear_ps_value);
    state->rgba_linear_cs_functional = run_rgba_cs_probe(
        state, state->rgba_srv, state->rgba_sample_compute_shader,
        state->linear_sampler, "LINEAR",
        &state->rgba_linear_cs_value);

    /* Confirm the updated DEFAULT image itself before using it as a second
     * sampled-image control. */
    {
        D3D11_MAPPED_SUBRESOURCE updated_mapped;
        ID3D11DeviceContext_CopyResource(
            state->context, (ID3D11Resource *)state->rgba_updated_staging,
            (ID3D11Resource *)state->rgba_updated_texture);
        state->feature_probe_gpu_copies++;
        ID3D11DeviceContext_Flush(state->context);
        result = ID3D11DeviceContext_Map(
            state->context, (ID3D11Resource *)state->rgba_updated_staging,
            0, D3D11_MAP_READ, 0, &updated_mapped);
        if (SUCCEEDED(result)) {
            state->rgba_updated_upload_value = rgba8_value(
                &updated_mapped, 0, 0);
            state->rgba_updated_upload_functional =
                state->rgba_updated_upload_value == WINEHUA_RGBA_EXPECTED;
            ID3D11DeviceContext_Unmap(
                state->context, (ID3D11Resource *)state->rgba_updated_staging, 0);
            state->feature_probe_read_bytes += 4 * 4 * 4;
        } else {
            state->rgba_updated_upload_value = WINEHUA_RGBA_MARKER;
            state->rgba_updated_upload_functional = FALSE;
        }
        fprintf(stderr, "winehua_d3d11_smoke: RGBA UPDATED upload value=0x%08x expected=0x%08x\\n",
                state->rgba_updated_upload_value, WINEHUA_RGBA_EXPECTED);
    }
    state->rgba_updated_load_ps_functional = run_rgba_ps_probe(
        state, state->rgba_updated_srv, state->rgba_load_pixel_shader,
        NULL, "UPDATED Load", &state->rgba_updated_load_ps_value);
    state->rgba_updated_load_cs_functional = run_rgba_cs_probe(
        state, state->rgba_updated_srv, state->rgba_load_compute_shader,
        NULL, "UPDATED Load", &state->rgba_updated_load_cs_value);
    state->rgba_updated_point_ps_functional = run_rgba_ps_probe(
        state, state->rgba_updated_srv, state->rgba_sample_pixel_shader,
        state->point_sampler, "UPDATED POINT", &state->rgba_updated_point_ps_value);
    state->rgba_updated_point_cs_functional = run_rgba_cs_probe(
        state, state->rgba_updated_srv, state->rgba_sample_compute_shader,
        state->point_sampler, "UPDATED POINT", &state->rgba_updated_point_cs_value);

    /* Four distinct immutable textures make descriptor slot permutation
     * visible in one readback.  The following passes also exercise immediate
     * context dirty-state updates, an explicitly unbound slot, and releasing
     * the application's references immediately after binding. */
    if (!run_descriptor_identity_probes(state))
        fprintf(stderr, "winehua_d3d11_smoke: descriptor identity matrix failed\n");
    if (!run_subresource_probes(state))
        fprintf(stderr, "winehua_d3d11_smoke: subresource matrix failed\n");

    ID3D11DeviceContext_OMSetRenderTargets(state->context, 1, targets, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(
        state->context, state->probe_rtv, (const float[]){0, 0, 0, 1});
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(
        state->context, state->bc_probe_pixel_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(state->context, 0, 1, bc_resources);
    ID3D11DeviceContext_PSSetSamplers(state->context, 0, 1, samplers);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(state->context, 0, 1, null_resources);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);
    ID3D11DeviceContext_CopyResource(
        state->context, (ID3D11Resource *)state->probe_staging,
        (ID3D11Resource *)state->probe_texture);
    state->feature_probe_gpu_copies++;

    ID3D11DeviceContext_CSSetShader(state->context, state->compute_shader, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(
        state->context, 0, 1, pattern_resources);
    ID3D11DeviceContext_CSSetSamplers(
        state->context, 0, 1, pattern_samplers);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(state->context, 0, 1, uavs, NULL);
    ID3D11DeviceContext_Dispatch(state->context, 2, 2, 1);
    state->compute_dispatch_ready = TRUE;
    ID3D11DeviceContext_CSSetShaderResources(
        state->context, 0, 1, null_srvs);
    ID3D11DeviceContext_CSSetSamplers(
        state->context, 0, 1, null_samplers);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(
        state->context, 0, 1, null_uavs, NULL);
    ID3D11DeviceContext_CSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_CopyResource(
        state->context, (ID3D11Resource *)state->compute_staging,
        (ID3D11Resource *)state->compute_texture);
    state->feature_probe_gpu_copies++;
    state->bc_sampling_submitted = TRUE;
    state->compute_uav_submitted = TRUE;
    ID3D11DeviceContext_Flush(state->context);

    result = ID3D11DeviceContext_Map(
        state->context, (ID3D11Resource *)state->probe_staging, 0,
        D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(result))
    {
        probe_readback_ok = check_probe_pixels(&mapped);
        if (!probe_readback_ok)
            fprintf(stderr, "winehua_d3d11_smoke: BC probe readback mismatch\\n");
        ID3D11DeviceContext_Unmap(
            state->context, (ID3D11Resource *)state->probe_staging, 0);
        state->feature_probe_read_bytes += WINEHUA_FEATURE_PROBE_SIZE *
                                           WINEHUA_FEATURE_PROBE_SIZE * 4;
    }
    else
        fprintf(stderr, "winehua_d3d11_smoke: BC probe readback unavailable=0x%08lx\\n",
                (unsigned long)result);
    state->offscreen_render_ready = TRUE;
    state->bc_sampling_functional = probe_readback_ok;

    /* Validate ordinary RGBA texture upload/update/sampling independently of
     * the BC emulation probe. This prevents a BC-only failure from being
     * reported as a generic texture sampling failure. */
    {
        ID3D11ShaderResourceView *pattern_resources[] = {state->pattern_srv};
        ID3D11SamplerState *pattern_samplers[] = {state->linear_sampler};
        ID3D11DeviceContext_OMSetRenderTargets(
            state->context, 1, targets, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(
            state->context, state->probe_rtv, (const float[]){0, 0, 0, 1});
        ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
        ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(
            state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(
            state->context, state->fullscreen_vertex_shader, NULL, 0);
        ID3D11DeviceContext_PSSetShader(
            state->context, state->bc_probe_pixel_shader, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(
            state->context, 0, 1, pattern_resources);
        ID3D11DeviceContext_PSSetSamplers(
            state->context, 0, 1, pattern_samplers);
            ID3D11DeviceContext_Draw(state->context, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(
            state->context, 0, 1, null_resources);
        ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);
        ID3D11DeviceContext_CopyResource(
            state->context, (ID3D11Resource *)state->probe_staging,
            (ID3D11Resource *)state->probe_texture);
        state->feature_probe_gpu_copies++;
        ID3D11DeviceContext_Flush(state->context);
        result = ID3D11DeviceContext_Map(
            state->context, (ID3D11Resource *)state->probe_staging, 0,
            D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(result))
        {
            state->texture_sampling_functional = check_pattern_pixels(&mapped);
            ID3D11DeviceContext_Unmap(
                state->context, (ID3D11Resource *)state->probe_staging, 0);
            state->feature_probe_read_bytes += WINEHUA_FEATURE_PROBE_SIZE *
                                               WINEHUA_FEATURE_PROBE_SIZE * 4;
        }
        else
            fprintf(stderr, "winehua_d3d11_smoke: pattern readback unavailable=0x%08lx\\n",
                    (unsigned long)result);
    }

    /* Exercise the actual D3D11 resolve operation, not merely the advertised
     * sample-count capability. Render the deterministic overlay into a 4x
     * target, resolve into the single-sample probe texture, then validate one
     * small readback point. This stays offscreen and does not add a per-frame
     * CPU transfer to the present path. */
    {
        ID3D11RenderTargetView *msaa_targets[] = {state->msaa_rtv};
        ID3D11DeviceContext_OMSetRenderTargets(
            state->context, 1, msaa_targets, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(
            state->context, state->msaa_rtv, (const float[]){0, 0, 0, 1});
        ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
        ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(
            state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(
            state->context, state->fullscreen_vertex_shader, NULL, 0);
        ID3D11DeviceContext_PSSetShader(
            state->context, state->stencil_overlay_pixel_shader, NULL, 0);
        ID3D11DeviceContext_Draw(state->context, 3, 0);
        ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);
        ID3D11DeviceContext_ResolveSubresource(
            state->context, (ID3D11Resource *)state->probe_texture, 0,
            (ID3D11Resource *)state->msaa_texture, 0,
            DXGI_FORMAT_R8G8B8A8_UNORM);
        state->feature_probe_gpu_copies++;
        ID3D11DeviceContext_CopyResource(
            state->context, (ID3D11Resource *)state->probe_staging,
            (ID3D11Resource *)state->probe_texture);
        state->feature_probe_gpu_copies++;
        ID3D11DeviceContext_Flush(state->context);
        result = ID3D11DeviceContext_Map(
            state->context, (ID3D11Resource *)state->probe_staging, 0,
            D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(result))
        {
            state->msaa_resolve_functional = check_msaa_resolve_pixels(&mapped);
            ID3D11DeviceContext_Unmap(
                state->context, (ID3D11Resource *)state->probe_staging, 0);
            state->feature_probe_read_bytes += WINEHUA_FEATURE_PROBE_SIZE *
                                               WINEHUA_FEATURE_PROBE_SIZE * 4;
        }
        else
            fprintf(stderr, "winehua_d3d11_smoke: MSAA resolve readback unavailable=0x%08lx\\n",
                    (unsigned long)result);
    }

    result = ID3D11DeviceContext_Map(
        state->context, (ID3D11Resource *)state->compute_staging, 0,
        D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(result))
    {
        compute_readback_ok = check_compute_pixels(
            &mapped, &state->compute_sampled_functional);
        if (!compute_readback_ok)
            fprintf(stderr, "winehua_d3d11_smoke: compute probe readback mismatch\\n");
        ID3D11DeviceContext_Unmap(
            state->context, (ID3D11Resource *)state->compute_staging, 0);
        state->feature_probe_read_bytes += 16 * 16 * 4;
    }
    else
        fprintf(stderr, "winehua_d3d11_smoke: compute probe readback unavailable=0x%08lx\\n",
                (unsigned long)result);
    state->compute_uav_functional = compute_readback_ok;
    return TRUE;
}

static BOOL feature_checks_passed(const struct smoke_state *state)
{
    return state->bc_texture_ready &&
           state->pattern_texture_ready &&
           state->texture_update_ready &&
           state->texture_upload_functional &&
           state->alpha_blend_ready &&
           state->stencil_test_ready &&
           state->cube_geometry_ready &&
           state->depth_stencil_ready &&
           state->constant_buffer_ready &&
           (!state->dynamic_constant_mode ||
            (state->dynamic_constant_test && state->dynamic_constant_readback)) &&
           state->rasterizer_state_ready &&
           state->shader_model_5_ready &&
           state->draw_indexed_instanced_ready &&
           state->offscreen_render_ready &&
           state->bc_sampling_functional &&
           state->msaa4x_supported &&
           state->msaa_resolve_functional &&
           state->compute_dispatch_ready &&
           state->compute_uav_submitted &&
           state->compute_uav_functional &&
           state->compute_sampled_functional &&
           state->texture_sampling_functional &&
           state->rgba_load_ps_functional &&
           state->rgba_load_cs_functional &&
           state->rgba_point_ps_functional &&
           state->rgba_point_cs_functional &&
           state->rgba_linear_ps_functional &&
           state->rgba_linear_cs_functional &&
           state->rgba_updated_upload_functional &&
           state->rgba_updated_load_ps_functional &&
           state->rgba_updated_load_cs_functional &&
           state->rgba_updated_point_ps_functional &&
           state->rgba_updated_point_cs_functional &&
           state->descriptor_identity_functional &&
           state->descriptor_rebind_functional &&
           state->descriptor_unbound_functional &&
           state->descriptor_lifetime_functional &&
           state->subresource_functional &&
           (!state->query_enabled ||
            (state->stencil_pixel_functional && state->stencil_functional));
}

static BOOL create_pipeline_states(struct smoke_state *state)
{
    D3D11_BLEND_DESC blend_desc = {0};
    D3D11_DEPTH_STENCIL_DESC depth_desc = {0};
    HRESULT result;

    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    result = ID3D11Device_CreateBlendState(state->device, &blend_desc,
                                           &state->blend_state);
    if (FAILED(result)) {
        state->present_result = result;
        return FALSE;
    }
    state->alpha_blend_ready = TRUE;

    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    depth_desc.StencilEnable = TRUE;
    depth_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    depth_desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
    depth_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depth_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    depth_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    depth_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    depth_desc.BackFace = depth_desc.FrontFace;
    result = ID3D11Device_CreateDepthStencilState(state->device, &depth_desc,
                                                  &state->depth_stencil_state);
    if (FAILED(result)) {
        state->present_result = result;
        return FALSE;
    }
    depth_desc.DepthEnable = FALSE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth_desc.StencilWriteMask = 0;
    depth_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depth_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    depth_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    depth_desc.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
    depth_desc.BackFace = depth_desc.FrontFace;
    result = ID3D11Device_CreateDepthStencilState(
        state->device, &depth_desc, &state->stencil_read_state);
    if (FAILED(result)) {
        state->present_result = result;
        return FALSE;
    }

    state->stencil_test_ready = TRUE;
    return TRUE;
}

static void build_cube_constants(UINT frame_count,
                                 struct cube_constants *constants);

static BOOL create_device(struct smoke_state *state)
{
    WNDCLASSA window_class = {0};
    DXGI_SWAP_CHAIN_DESC swap_desc = {0};
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0,
                                  D3D_FEATURE_LEVEL_9_3};
    const char *vertex_source =
        "cbuffer CubeConstants : register(b0) { row_major float4x4 transform; };"
        "struct VSIn { float3 pos : POSITION; float4 color : COLOR; float2 uv : TEXCOORD0; };"
        "struct VSOut { float4 pos : SV_Position; float4 color : COLOR;"
        "float2 uv : TEXCOORD0; nointerpolation uint instance_id : TEXCOORD1; };"
        "VSOut main(VSIn input, uint instance_id : SV_InstanceID) { VSOut output;"
        "float4 clip = mul(float4(input.pos * 0.72, 1.0), transform);"
        "clip.x += (instance_id == 0 ? -0.30 : 0.30) * clip.w;"
        "output.pos = clip; output.color = input.color; output.uv = input.uv;"
        "output.instance_id = instance_id; return output; }";
    const char *pixel_source =
        "Texture2D probeTexture : register(t0);"
        "Texture2D patternTexture : register(t1);"
        "Texture2D computeTexture : register(t2);"
        "SamplerState pointSampler : register(s0);"
        "SamplerState linearSampler : register(s1);"
        "struct PSIn { float4 pos : SV_Position; float4 color : COLOR;"
        "float2 uv : TEXCOORD0; nointerpolation uint instance_id : TEXCOORD1; };"
        "float4 main(PSIn input) : SV_Target { float3 base;"
        "if (input.instance_id == 0) {"
        "float3 decoded = probeTexture.Sample(pointSampler, input.uv).rgb;"
        "float3 pattern = patternTexture.Sample(linearSampler, input.uv).rgb;"
        "base = saturate(input.color.rgb * 0.55 + decoded * 0.55 + pattern * 0.35); } else {"
        "base = saturate(input.color.rgb * 0.55 + computeTexture.Sample(pointSampler, input.uv * 2.0).rgb * 0.65); }"
        "float3 faceTint = 0.55 + 0.45 * input.color.rgb;"
        "if (input.pos.x > 320.0) base = saturate(base * 0.25 + float3(0.0, 0.70, 0.08));"
        "return float4(saturate(base * faceTint), 0.92); }";
    ID3DBlob *vertex_blob = NULL, *pixel_blob = NULL;
    D3D11_INPUT_ELEMENT_DESC elements[3] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    HRESULT result;

    state->dynamic_constant_mode = !strcmp(
        winehua_smoke_env("WINEHUA_D3D11_DYNAMIC_CB", "0"), "1");
    state->query_enabled = !strcmp(
        winehua_smoke_env("WINEHUA_D3D11_STENCIL_QUERY", "0"), "1");
    if (!strcmp(winehua_smoke_env("WINEHUA_D3D11_COMBINED_SAMPLER", "0"), "1"))
        SetEnvironmentVariableA("WINEHUA_DXVK_COMBINED_SAMPLER", "1");

    window_class.lpfnWndProc = wndproc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.lpszClassName = "WineHuaD3D11SmokeWindow";
    if (!RegisterClassA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return FALSE;
    state->hwnd = CreateWindowExA(0, window_class.lpszClassName, "WineHua DXVK smoke",
                                  WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL,
                                  window_class.hInstance, NULL);
    if (!state->hwnd) return FALSE;
    ShowWindow(state->hwnd, SW_SHOWNOACTIVATE);
    Sleep(500);

    swap_desc.BufferCount = 2;
    swap_desc.BufferDesc.Width = 640;
    swap_desc.BufferDesc.Height = 480;
    swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.OutputWindow = state->hwnd;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.Windowed = TRUE;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    result = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                           levels, sizeof(levels) / sizeof(levels[0]),
                                           D3D11_SDK_VERSION, &swap_desc,
                                           &state->swapchain, &state->device,
                                           &state->feature_level, &state->context);
    if (FAILED(result))
    {
        state->present_result = result;
        return FALSE;
    }
    {
        D3D11_TEXTURE2D_DESC probe_desc;
        result = IDXGISwapChain_GetBuffer(state->swapchain, 0,
                                          &WINEHUA_IID_ID3D11Texture2D,
                                          (void **)&state->backbuffer);
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateRenderTargetView(state->device,
                                                         (ID3D11Resource *)state->backbuffer,
                                                         NULL, &state->rtv);
        if (SUCCEEDED(result))
        {
            ID3D11Texture2D_GetDesc(state->backbuffer, &probe_desc);
            probe_desc.Width = 1;
            probe_desc.Height = 1;
            probe_desc.MipLevels = 1;
            probe_desc.ArraySize = 1;
            probe_desc.SampleDesc.Count = 1;
            probe_desc.SampleDesc.Quality = 0;
            probe_desc.Usage = D3D11_USAGE_STAGING;
            probe_desc.BindFlags = 0;
            probe_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            probe_desc.MiscFlags = 0;
            result = ID3D11Device_CreateTexture2D(
                state->device, &probe_desc, NULL, &state->stencil_probe_staging);
        }
        if (FAILED(result)) { state->present_result = result; return FALSE; }
    }
    {
        D3D11_TEXTURE2D_DESC depth_desc = {0};
        ID3D11Texture2D *depth_texture = NULL;
        depth_desc.Width = 640;
        depth_desc.Height = 480;
        depth_desc.MipLevels = 1;
        depth_desc.ArraySize = 1;
        depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count = 1;
        depth_desc.Usage = D3D11_USAGE_DEFAULT;
        depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        result = ID3D11Device_CreateTexture2D(state->device, &depth_desc, NULL,
                                               &depth_texture);
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateDepthStencilView(
                state->device, (ID3D11Resource *)depth_texture, NULL, &state->dsv);
        if (depth_texture) ID3D11Texture2D_Release(depth_texture);
        if (FAILED(result)) { state->present_result = result; return FALSE; }
        state->depth_stencil_ready = TRUE;
    }
    if (!compile_shader(vertex_source, "main", "vs_5_0", &vertex_blob) ||
        !compile_shader(pixel_source, "main", "ps_5_0", &pixel_blob))
    {
        state->present_result = E_FAIL;
        if (vertex_blob) ID3D10Blob_Release(vertex_blob);
        if (pixel_blob) ID3D10Blob_Release(pixel_blob);
        return FALSE;
    }
    result = ID3D11Device_CreateVertexShader(state->device,
        ID3D10Blob_GetBufferPointer(vertex_blob), ID3D10Blob_GetBufferSize(vertex_blob),
        NULL, &state->vertex_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreatePixelShader(state->device,
            ID3D10Blob_GetBufferPointer(pixel_blob), ID3D10Blob_GetBufferSize(pixel_blob),
            NULL, &state->pixel_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateInputLayout(state->device, elements, 3,
            ID3D10Blob_GetBufferPointer(vertex_blob), ID3D10Blob_GetBufferSize(vertex_blob),
            &state->input_layout);
    if (vertex_blob) ID3D10Blob_Release(vertex_blob);
    if (pixel_blob) ID3D10Blob_Release(pixel_blob);
    if (FAILED(result)) { state->present_result = result; return FALSE; }

    state->shader_model_5_ready = TRUE;

    if (!create_bc_texture(state)) return FALSE;
    if (!create_pattern_texture(state)) return FALSE;
    if (!create_rgba_sample_texture(state)) return FALSE;
    if (!create_descriptor_identity_resources(state)) return FALSE;
    if (!create_pipeline_states(state)) return FALSE;

    if (!create_feature_shaders(state) ||
        !create_feature_resources(state) ||
        !run_feature_probes(state))
        return FALSE;

    {
        D3D11_RASTERIZER_DESC rasterizer_desc = {0};
        rasterizer_desc.FillMode = D3D11_FILL_SOLID;
        rasterizer_desc.CullMode = D3D11_CULL_NONE;
        rasterizer_desc.DepthClipEnable = TRUE;
        result = ID3D11Device_CreateRasterizerState(
            state->device, &rasterizer_desc, &state->rasterizer_state);
        if (FAILED(result)) { state->present_result = result; return FALSE; }
        state->rasterizer_state_ready = TRUE;
    }

    {
        static const struct vertex vertices[] = {
            /* front */ {-1,-1,-1, 1,0,0,1, 0,1}, { -1,1,-1, 1,0,0,1, 0,0},
            { 1,1,-1, 1,0,0,1, 1,0}, { 1,-1,-1, 1,0,0,1, 1,1},
            /* back */ { 1,-1,1, 0,1,0,1, 0,1}, { 1,1,1, 0,1,0,1, 0,0},
            {-1,1,1, 0,1,0,1, 1,0}, {-1,-1,1, 0,1,0,1, 1,1},
            /* left */ {-1,-1,1, 0,0,1,1, 0,1}, {-1,1,1, 0,0,1,1, 0,0},
            {-1,1,-1, 0,0,1,1, 1,0}, {-1,-1,-1, 0,0,1,1, 1,1},
            /* right */ {1,-1,-1, 0,1,0,1, 0,1}, {1,1,-1, 0,1,0,1, 0,0},
            {1,1,1, 0,1,0,1, 1,0}, {1,-1,1, 0,1,0,1, 1,1},
            /* top */ {-1,1,-1, 1,0,1,1, 0,1}, {-1,1,1, 1,0,1,1, 0,0},
            {1,1,1, 1,0,1,1, 1,0}, {1,1,-1, 1,0,1,1, 1,1},
            /* bottom */ {-1,-1,1, 0,1,1,1, 0,1}, {-1,-1,-1, 0,1,1,1, 0,0},
            {1,-1,-1, 0,1,1,1, 1,0}, {1,-1,1, 0,1,1,1, 1,1},
        };
        static const unsigned short indices[] = {
            0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
            12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23
        };
        D3D11_BUFFER_DESC desc = {sizeof(vertices), D3D11_USAGE_IMMUTABLE,
                                  D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA data = {vertices, 0, 0};
        result = ID3D11Device_CreateBuffer(state->device, &desc, &data,
                                            &state->vertex_buffer);
        if (FAILED(result)) { state->present_result = result; return FALSE; }
        desc.ByteWidth = sizeof(indices);
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        data.pSysMem = indices;
        result = ID3D11Device_CreateBuffer(state->device, &desc, &data,
                                            &state->index_buffer);
        if (FAILED(result)) { state->present_result = result; return FALSE; }
        desc.ByteWidth = sizeof(struct cube_constants);
        desc.Usage = state->dynamic_constant_mode ? D3D11_USAGE_DYNAMIC
                                                   : D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = state->dynamic_constant_mode
                                 ? D3D11_CPU_ACCESS_WRITE : 0;
        struct cube_constants initial_constants;
        build_cube_constants(0, &initial_constants);
        data.pSysMem = state->dynamic_constant_mode ? NULL : &initial_constants;
        result = ID3D11Device_CreateBuffer(
            state->device, &desc,
            state->dynamic_constant_mode ? NULL : &data,
                                            &state->constant_buffer);
        if (FAILED(result)) { state->present_result = result; return FALSE; }
        if (state->dynamic_constant_mode)
        {
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            result = ID3D11Device_CreateBuffer(
                state->device, &desc, NULL, &state->dynamic_constant_staging);
            if (FAILED(result)) { state->present_result = result; return FALSE; }
        }
        state->cube_geometry_ready = TRUE;
        state->constant_buffer_ready = TRUE;
    }
    state->present_result = S_OK;
    return TRUE;
}

static void matrix_identity(float *matrix)
{
    memset(matrix, 0, sizeof(float) * 16);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

static void matrix_multiply(float *out, const float *a, const float *b)
{
    float result[16];
    unsigned int row, column, index;
    for (row = 0; row < 4; ++row)
        for (column = 0; column < 4; ++column)
        {
            result[row * 4 + column] = 0.0f;
            for (index = 0; index < 4; ++index)
                result[row * 4 + column] += a[row * 4 + index] * b[index * 4 + column];
        }
    memcpy(out, result, sizeof(result));
}

static void build_cube_constants(UINT frame_count,
                                 struct cube_constants *constants)
{
    float rotation_x[16], rotation_y[16], translation[16];
    float projection[16], world[16], view[16];
    float cy, sy;

    switch ((frame_count / 30) % 4)
    {
    case 1: cy = 0.7071067f; sy = 0.7071067f; break;
    case 2: cy = 0.0f; sy = 1.0f; break;
    case 3: cy = -0.7071067f; sy = 0.7071067f; break;
    default: cy = 1.0f; sy = 0.0f; break;
    }
    matrix_identity(rotation_x);
    rotation_x[5] = 0.9396926f;
    rotation_x[6] = -0.3420201f;
    rotation_x[9] = 0.3420201f;
    rotation_x[10] = 0.9396926f;
    matrix_identity(rotation_y);
    rotation_y[0] = cy;
    rotation_y[2] = -sy;
    rotation_y[8] = sy;
    rotation_y[10] = cy;
    matrix_identity(translation);
    translation[14] = 4.0f;
    matrix_identity(projection);
    projection[0] = 1.2990381f;
    projection[5] = 1.7320508f;
    projection[10] = 100.0f / 99.9f;
    projection[11] = 1.0f;
    projection[14] = -0.1f * 100.0f / 99.9f;
    matrix_multiply(world, rotation_x, rotation_y);
    matrix_multiply(view, world, translation);
    matrix_multiply(constants->transform, view, projection);
}

static BOOL update_cube_transform(struct smoke_state *state)
{
    struct cube_constants constants;
    build_cube_constants(state->frame_count, &constants);
    if (state->dynamic_constant_mode)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT result = ID3D11DeviceContext_Map(
            state->context, (ID3D11Resource *)state->constant_buffer, 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result))
            return FALSE;
        memcpy(mapped.pData, &constants, sizeof(constants));
        ID3D11DeviceContext_Unmap(
            state->context, (ID3D11Resource *)state->constant_buffer, 0);
        state->dynamic_constant_test = TRUE;
        if (state->frame_count == 30)
        {
            ID3D11DeviceContext_CopyResource(
                state->context, (ID3D11Resource *)state->dynamic_constant_staging,
                (ID3D11Resource *)state->constant_buffer);
            state->feature_probe_gpu_copies++;
            result = ID3D11DeviceContext_Map(
                state->context, (ID3D11Resource *)state->dynamic_constant_staging,
                0, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(result)) return FALSE;
            state->dynamic_constant_readback =
                !memcmp(mapped.pData, &constants, sizeof(constants));
            fprintf(stderr,
                    "winehua_d3d11_smoke: dynamic constant readback=%u words=0x%08x,0x%08x,0x%08x,0x%08x\\n",
                    state->dynamic_constant_readback,
                    ((const UINT *)mapped.pData)[0], ((const UINT *)mapped.pData)[1],
                    ((const UINT *)mapped.pData)[2], ((const UINT *)mapped.pData)[3]);
            ID3D11DeviceContext_Unmap(
                state->context, (ID3D11Resource *)state->dynamic_constant_staging, 0);
            state->feature_probe_read_bytes += sizeof(constants);
        }
    }
    return TRUE;
}

static void poll_stencil_query(struct smoke_state *state, DWORD wait_ms)
{
    UINT64 samples = 0;
    HRESULT result = S_FALSE;
    DWORD deadline;

    if (!state->stencil_query_pending) return;

    deadline = GetTickCount() + wait_ms;
    do
    {
        result = ID3D11DeviceContext_GetData(
            state->context, (ID3D11Asynchronous *)state->stencil_query,
            &samples, sizeof(samples), 0);
        if (result != S_FALSE) break;
        if (!wait_ms) return;
        Sleep(1);
    } while (GetTickCount() < deadline);

    if (result == S_FALSE)
    {
        fprintf(stderr, "winehua_d3d11_smoke: stencil query still pending after %lu ms\\n",
                (unsigned long)wait_ms);
        return;
    }

    state->stencil_query_pending = FALSE;
    state->stencil_samples = samples;
    state->stencil_functional = result == S_OK &&
        samples > 1000 && samples < 260000;
    fprintf(stderr, "winehua_d3d11_smoke: stencil query result=0x%08lx samples=%llu\\n",
            (unsigned long)result, (unsigned long long)samples);
}

static BOOL verify_stencil_pixel(struct smoke_state *state)
{
    D3D11_BOX source_box = {224, 240, 0, 225, 241, 1};
    D3D11_MAPPED_SUBRESOURCE mapped;
    const BYTE *pixel;
    HRESULT result;

    ID3D11DeviceContext_CopySubresourceRegion(
        state->context, (ID3D11Resource *)state->stencil_probe_staging,
        0, 0, 0, 0, (ID3D11Resource *)state->backbuffer, 0, &source_box);
    state->feature_probe_gpu_copies++;
    result = ID3D11DeviceContext_Map(
        state->context, (ID3D11Resource *)state->stencil_probe_staging,
        0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        fprintf(stderr, "winehua_d3d11_smoke: stencil pixel readback failed=0x%08lx\\n",
                (unsigned long)result);
        return FALSE;
    }

    pixel = mapped.pData;
    state->stencil_pixel_functional =
        pixel[0] > 32 || pixel[1] > 32 || pixel[2] > 32;
    fprintf(stderr, "winehua_d3d11_smoke: stencil pixel=%u,%u,%u,%u functional=%u\\n",
            pixel[0], pixel[1], pixel[2], pixel[3],
            state->stencil_pixel_functional);
    ID3D11DeviceContext_Unmap(
        state->context, (ID3D11Resource *)state->stencil_probe_staging, 0);
    state->feature_probe_read_bytes += 4;
    return TRUE;
}

static BOOL draw_frame(struct smoke_state *state)
{
    MSG message;
    UINT stride = sizeof(struct vertex), offset = 0;
    ID3D11RenderTargetView *targets[] = {state->rtv};
    D3D11_VIEWPORT viewport = {0, 0, 640, 480, 0.0f, 1.0f};
    ID3D11Buffer *constant_buffers[] = {state->constant_buffer};
    ID3D11ShaderResourceView *resources[] = {
        state->probe_srv, state->pattern_srv, state->compute_srv};
    ID3D11ShaderResourceView *null_resources[] = {NULL, NULL, NULL};
    ID3D11SamplerState *samplers[] = {state->bc_sampler, state->linear_sampler};
    UINT query_frame = state->dynamic_constant_mode ? 30 : 0;
    BOOL query_active = state->query_enabled && state->stencil_query &&
                        state->frame_count == query_frame;
    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 1, targets, state->dsv);
    ID3D11DeviceContext_ClearRenderTargetView(state->context, state->rtv,
                                              (const float[]){0.03f, 0.03f, 0.05f, 1.0f});
    ID3D11DeviceContext_ClearDepthStencilView(state->context, state->dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                              1.0f, 0);
    ID3D11DeviceContext_OMSetDepthStencilState(state->context,
                                               state->depth_stencil_state, 1);
    ID3D11DeviceContext_OMSetBlendState(state->context, state->blend_state,
                                        NULL, 0xffffffffu);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_RSSetState(state->context, state->rasterizer_state);
    if (!update_cube_transform(state)) return FALSE;
    ID3D11DeviceContext_IASetInputLayout(state->context, state->input_layout);
    ID3D11DeviceContext_IASetVertexBuffers(state->context, 0, 1,
                                           &state->vertex_buffer, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(state->context, state->index_buffer,
                                         DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(state->context,
                                               D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(state->context, state->vertex_shader, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(state->context, 0, 1, constant_buffers);
    ID3D11DeviceContext_PSSetShader(state->context, state->pixel_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(state->context, 0, 3, resources);
    ID3D11DeviceContext_PSSetSamplers(state->context, 0, 2, samplers);
    ID3D11DeviceContext_DrawIndexedInstanced(state->context, 36, 2, 0, 0, 0);
    state->draw_indexed_instanced_ready = TRUE;

    /* A fullscreen triangle is accepted only where the cube wrote stencil.
     * The occlusion query turns this into a functional stencil-read test. */
    ID3D11DeviceContext_PSSetShaderResources(
        state->context, 0, 3, null_resources);
    if (query_active)
    {
        fprintf(stderr, "winehua_d3d11_smoke: stencil query begin frame=%u dynamic=%u\\n",
                state->frame_count, state->dynamic_constant_mode);
        ID3D11DeviceContext_Begin(state->context,
                                  (ID3D11Asynchronous *)state->stencil_query);
    }
    ID3D11DeviceContext_OMSetDepthStencilState(
        state->context, state->stencil_read_state, 1);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(
        state->context, state->stencil_overlay_pixel_shader, NULL, 0);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    if (query_active)
    {
        ID3D11DeviceContext_End(state->context,
                                (ID3D11Asynchronous *)state->stencil_query);
        state->stencil_query_pending = TRUE;
        verify_stencil_pixel(state);
    }

    ID3D11DeviceContext_Flush(state->context);
    state->present_result = IDXGISwapChain_Present(state->swapchain, 1, 0);
    if (FAILED(state->present_result)) return FALSE;
    poll_stencil_query(state, 0);
    state->frame_count++;
    return TRUE;
}

static void release_state(struct smoke_state *state)
{
    UINT i;
    /* Complete queued Venus/DXVK work before the next Wine process reuses the
     * host Vulkan context. Clear bindings first, then give the broker a short
     * bounded handoff window. */
    if (state->context)
    {
        ID3D11DeviceContext_ClearState(state->context);
        ID3D11DeviceContext_Flush(state->context);
        Sleep(250);
    }
    if (state->probe_rtv) ID3D11RenderTargetView_Release(state->probe_rtv);
    if (state->stencil_query) ID3D11Query_Release(state->stencil_query);
    if (state->stencil_probe_staging)
        ID3D11Texture2D_Release(state->stencil_probe_staging);
    if (state->backbuffer) ID3D11Texture2D_Release(state->backbuffer);
    if (state->compute_staging) ID3D11Texture2D_Release(state->compute_staging);
    if (state->compute_uav) ID3D11UnorderedAccessView_Release(state->compute_uav);
    if (state->compute_srv) ID3D11ShaderResourceView_Release(state->compute_srv);
    if (state->compute_texture) ID3D11Texture2D_Release(state->compute_texture);
    if (state->subresource_srv)
        ID3D11ShaderResourceView_Release(state->subresource_srv);
    if (state->subresource_texture)
        ID3D11Texture2D_Release(state->subresource_texture);
    if (state->probe_staging) ID3D11Texture2D_Release(state->probe_staging);
    if (state->probe_srv) ID3D11ShaderResourceView_Release(state->probe_srv);
    if (state->probe_texture) ID3D11Texture2D_Release(state->probe_texture);
    if (state->msaa_rtv) ID3D11RenderTargetView_Release(state->msaa_rtv);
    if (state->msaa_texture) ID3D11Texture2D_Release(state->msaa_texture);
    if (state->stencil_read_state)
        ID3D11DepthStencilState_Release(state->stencil_read_state);
    if (state->stencil_overlay_pixel_shader)
        ID3D11PixelShader_Release(state->stencil_overlay_pixel_shader);
    if (state->rgba_load_pixel_shader)
        ID3D11PixelShader_Release(state->rgba_load_pixel_shader);
    if (state->descriptor_identity_pixel_shader)
        ID3D11PixelShader_Release(state->descriptor_identity_pixel_shader);
    if (state->subresource_pixel_shader)
        ID3D11PixelShader_Release(state->subresource_pixel_shader);
    if (state->rgba_sample_pixel_shader)
        ID3D11PixelShader_Release(state->rgba_sample_pixel_shader);
    if (state->bc_probe_pixel_shader)
        ID3D11PixelShader_Release(state->bc_probe_pixel_shader);
    if (state->fullscreen_vertex_shader)
        ID3D11VertexShader_Release(state->fullscreen_vertex_shader);
    if (state->compute_shader)
        ID3D11ComputeShader_Release(state->compute_shader);
    if (state->rgba_load_compute_shader)
        ID3D11ComputeShader_Release(state->rgba_load_compute_shader);
    if (state->rgba_sample_compute_shader)
        ID3D11ComputeShader_Release(state->rgba_sample_compute_shader);
    if (state->constant_buffer) ID3D11Buffer_Release(state->constant_buffer);
    if (state->dynamic_constant_staging)
        ID3D11Buffer_Release(state->dynamic_constant_staging);
    if (state->index_buffer) ID3D11Buffer_Release(state->index_buffer);
    if (state->vertex_buffer) ID3D11Buffer_Release(state->vertex_buffer);
    if (state->input_layout) ID3D11InputLayout_Release(state->input_layout);
    if (state->vertex_shader) ID3D11VertexShader_Release(state->vertex_shader);
    if (state->pixel_shader) ID3D11PixelShader_Release(state->pixel_shader);
    if (state->bc_sampler) ID3D11SamplerState_Release(state->bc_sampler);
    if (state->point_sampler) ID3D11SamplerState_Release(state->point_sampler);
    if (state->linear_sampler) ID3D11SamplerState_Release(state->linear_sampler);
    if (state->blend_state) ID3D11BlendState_Release(state->blend_state);
    if (state->depth_stencil_state)
        ID3D11DepthStencilState_Release(state->depth_stencil_state);
    if (state->rasterizer_state) ID3D11RasterizerState_Release(state->rasterizer_state);
    if (state->pattern_srv) ID3D11ShaderResourceView_Release(state->pattern_srv);
    if (state->pattern_staging) ID3D11Texture2D_Release(state->pattern_staging);
    if (state->pattern_texture) ID3D11Texture2D_Release(state->pattern_texture);
    if (state->rgba_srv) ID3D11ShaderResourceView_Release(state->rgba_srv);
    if (state->rgba_texture) ID3D11Texture2D_Release(state->rgba_texture);
    if (state->rgba_updated_staging)
        ID3D11Texture2D_Release(state->rgba_updated_staging);
    if (state->rgba_updated_srv)
        ID3D11ShaderResourceView_Release(state->rgba_updated_srv);
    if (state->rgba_updated_texture)
        ID3D11Texture2D_Release(state->rgba_updated_texture);
    for (i = 0; i < 4; ++i)
    {
        if (state->descriptor_srvs[i])
            ID3D11ShaderResourceView_Release(state->descriptor_srvs[i]);
        if (state->descriptor_textures[i])
            ID3D11Texture2D_Release(state->descriptor_textures[i]);
    }
    if (state->bc_srv) ID3D11ShaderResourceView_Release(state->bc_srv);
    if (state->bc_texture) ID3D11Texture2D_Release(state->bc_texture);
    if (state->dsv) ID3D11DepthStencilView_Release(state->dsv);
    if (state->rtv) ID3D11RenderTargetView_Release(state->rtv);
    if (state->context) ID3D11DeviceContext_Release(state->context);
    if (state->swapchain) IDXGISwapChain_Release(state->swapchain);
    if (state->device) ID3D11Device_Release(state->device);
    if (state->hwnd) DestroyWindow(state->hwnd);
}

int main(int argc, char **argv)
{
    struct smoke_state state;
    UINT frame_target;
    memset(&state, 0, sizeof(state));
    state.started_ms = winehua_smoke_timestamp_ms();
    state.present_result = E_FAIL;
    if (!winehua_smoke_parse_options(&state.smoke, argc, argv, 1)) return 6;
    if (!state.smoke.present)
    {
        write_state(&state, "UNSUPPORTED", "dxvk", "D3D11 smoke requires Win32 present");
        return 3;
    }
    write_state(&state, "STARTED", "startup", "DXVK Legacy D3D11 smoke starting");
    if (!create_device(&state))
    {
        write_state(&state, "FAIL", "dxvk", "D3D11 device or shader creation failed");
        release_state(&state);
        return 1;
    }
    frame_target = state.smoke.seconds ? state.smoke.seconds * 30 : 30;
    while (state.frame_count < frame_target)
    {
        if (!draw_frame(&state))
        {
            state.present_failure_frame = state.frame_count;
            write_state(&state, "FAIL", "present", "DXVK D3D11 Present failed");
            release_state(&state);
            return 1;
        }
        if (state.frame_count == 60)
        {
            write_state(&state, "RUNNING", "dxvk", "fixed-frame");
            /* Keep the fixed frame alive while the App compositor performs
             * snapshot_display.  The result JSON remains authoritative; this
             * bounded hold only makes the visual gate deterministic. */
            Sleep(state.smoke.automation ? 6000 : 2000);
            if (state.smoke.automation) break;
        }
    }
    poll_stencil_query(&state, 2000);
    if (!dxvk_modules_loaded(&state))
    {
        write_state(&state, "FAIL", "dxvk", "D3D11/DXGI did not load from the selected DXVK runtime");
        release_state(&state);
        return 1;
    }
    if (!feature_checks_passed(&state))
    {
        write_state(&state, "FAIL", "dxvk",
                    "D3D11 functional feature gate failed");
        release_state(&state);
        return 1;
    }
    write_state(&state, "PASS", "dxvk", "DXVK Legacy D3D11 fixed-frame check passed");
    release_state(&state);
    return 0;
}
