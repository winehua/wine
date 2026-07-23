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
#define WINEHUA_TEXTURE3D_SIZE 4
#define WINEHUA_TEXTURE3D_TEXELS (WINEHUA_TEXTURE3D_SIZE * WINEHUA_TEXTURE3D_SIZE * WINEHUA_TEXTURE3D_SIZE)
#define WINEHUA_BC_MATRIX_FORMATS 8
#define WINEHUA_HEAVEN_CUBE_FORMATS 5
#define WINEHUA_D24_ARRAY_LAYERS 6
#define WINEHUA_D24_CUBE_ARRAY_LAYERS 12
#define WINEHUA_D24_BORDER_CASES 4
#define WINEHUA_D24_EXTENDED_CASES 7
#define WINEHUA_MRT_PROBE_POINTS 4
#define WINEHUA_D24_DEPTH_POINTS 3
#define WINEHUA_D24_COMPARE_POINTS 9
#define WINEHUA_RGBA16F_POINTS 6
#define WINEHUA_HEAVEN_MINI_POINTS 4

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
    ID3D11PixelShader *sampler_pair_pixel_shader;
    ID3D11PixelShader *subresource_pixel_shader;
    ID3D11PixelShader *stencil_overlay_pixel_shader;
    ID3D11ComputeShader *compute_shader;
    ID3D11ComputeShader *rgba_sample_compute_shader;
    ID3D11ComputeShader *rgba_load_compute_shader;
    ID3D11ComputeShader *texture3d_pingpong_compute_shader;
    ID3D11ComputeShader *texture3d_oob_compute_shader;
    ID3D11ComputeShader *texture3d_border_compute_shader;
    ID3D11Texture2D *bc_texture;
    ID3D11ShaderResourceView *bc_srv;
    ID3D11SamplerState *bc_sampler;
    ID3D11Texture2D *bc_matrix_textures[WINEHUA_BC_MATRIX_FORMATS];
    ID3D11ShaderResourceView *bc_matrix_srvs[WINEHUA_BC_MATRIX_FORMATS];
    ID3D11SamplerState *bc_mip_sampler;
    ID3D11Texture2D *pattern_texture;
    ID3D11Texture2D *pattern_staging;
    ID3D11ShaderResourceView *pattern_srv;
    ID3D11SamplerState *linear_sampler;
    ID3D11SamplerState *border_point_sampler;
    ID3D11SamplerState *border_linear_sampler;
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
    ID3D11Texture3D *texture3d[2];
    ID3D11ShaderResourceView *texture3d_srvs[2];
    ID3D11UnorderedAccessView *texture3d_uavs[2];
    ID3D11Texture3D *texture3d_staging;
    ID3D11Texture3D *texture3d_border;
    ID3D11ShaderResourceView *texture3d_border_srv;
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
    BOOL bc_matrix_functional;
    BOOL msaa4x_supported;
    BOOL msaa_resolve_functional;
    BOOL compute_dispatch_ready;
    BOOL compute_uav_submitted;
    BOOL compute_uav_functional;
    BOOL compute_sampled_functional;
    BOOL texture3d_created;
    BOOL texture3d_upload_functional;
    BOOL texture3d_single_dispatch_functional;
    BOOL texture3d_uav_to_srv_functional;
    BOOL texture3d_pingpong_functional;
    BOOL texture3d_oob_load_functional;
    BOOL texture3d_oob_index_functional;
    BOOL texture3d_border_point_functional;
    BOOL texture3d_border_linear_functional;
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
    BOOL sampler_pair_functional;
    BOOL subresource_array_functional;
    BOOL subresource_mip_functional;
    BOOL subresource_explicit_lod_functional;
    BOOL subresource_barrier_functional;
    BOOL subresource_functional;
    BOOL heaven_cube_cases[WINEHUA_HEAVEN_CUBE_FORMATS];
    BOOL heaven_cube_functional;
    BOOL heaven_texture3d_r8_functional;
    BOOL heaven_texture3d_rg8_functional;
    BOOL heaven_comparison_sampler_functional;
    BOOL heaven_depth_comparison_sampler_functional;
    BOOL heaven_d24s8_depth_comparison_sampler_functional;
    BOOL heaven_d24s8_array_functional;
    BOOL heaven_d24s8_array_views_functional;
    BOOL heaven_d24s8_cube_as_array_functional;
    BOOL heaven_d24s8_cube_sample_functional;
    BOOL heaven_d24s8_cube_functional;
    BOOL heaven_d24s8_cube_array_functional;
    BOOL heaven_d24s8_linear_border_functional;
    BOOL heaven_d24s8_extended_functional;
    BOOL heaven_resource_functional;
    BOOL mrt_gbuffer_functional;
    BOOL d24_readonly_shadow_functional;
    BOOL rgba16f_rtv_srv_load_functional;
    BOOL heaven_mini_pipeline_functional;
    BOOL heaven_pass_functional;
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
    UINT bc_matrix_values[WINEHUA_BC_MATRIX_FORMATS][2];
    UINT descriptor_identity_values[4];
    UINT descriptor_rebind_values[4];
    UINT descriptor_unbound_values[4];
    UINT descriptor_lifetime_values[4];
    UINT sampler_pair_values[4];
    UINT subresource_initial_values[4];
    UINT subresource_updated_values[4];
    UINT heaven_cube_mismatches[WINEHUA_HEAVEN_CUBE_FORMATS];
    UINT heaven_texture3d_r8_values[4];
    UINT heaven_texture3d_rg8_values[4];
    UINT heaven_comparison_sampler_values[4];
    UINT heaven_depth_comparison_sampler_values[4];
    UINT heaven_d24s8_depth_comparison_sampler_values[4];
    UINT heaven_d24s8_array_values[WINEHUA_D24_ARRAY_LAYERS];
    UINT heaven_d24s8_array_view_values[WINEHUA_D24_ARRAY_LAYERS];
    UINT heaven_d24s8_cube_as_array_values[WINEHUA_D24_ARRAY_LAYERS];
    UINT heaven_d24s8_cube_sample_values[WINEHUA_D24_ARRAY_LAYERS];
    UINT heaven_d24s8_cube_values[WINEHUA_D24_ARRAY_LAYERS];
    UINT heaven_d24s8_cube_array_values[WINEHUA_D24_CUBE_ARRAY_LAYERS];
    UINT heaven_d24s8_linear_border_values[WINEHUA_D24_BORDER_CASES];
    UINT heaven_d24s8_extended_mismatches[WINEHUA_D24_EXTENDED_CASES];
    UINT mrt_gbuffer_values[WINEHUA_MRT_PROBE_POINTS];
    UINT d24_initial_values[WINEHUA_D24_DEPTH_POINTS];
    UINT d24_compare_values[WINEHUA_D24_COMPARE_POINTS];
    UINT d24_rewrite_values[WINEHUA_D24_DEPTH_POINTS];
    UINT rgba16f_initial_values[WINEHUA_RGBA16F_POINTS];
    UINT rgba16f_added_values[WINEHUA_RGBA16F_POINTS];
    UINT rgba16f_tonemap_values[WINEHUA_RGBA16F_POINTS];
    UINT heaven_mini_pipeline_values[WINEHUA_HEAVEN_MINI_POINTS];
    UINT mrt_gbuffer_mismatches;
    UINT d24_readonly_shadow_mismatches;
    UINT rgba16f_rtv_srv_load_mismatches;
    UINT heaven_mini_pipeline_mismatches;
    UINT texture3d_upload_mismatches;
    UINT texture3d_single_mismatches;
    UINT texture3d_pingpong_mismatches;
    UINT texture3d_oob_load_mismatches;
    UINT texture3d_oob_index_mismatches;
    UINT texture3d_border_point_mismatches;
    UINT texture3d_border_linear_mismatches;
    UINT texture3d_oob_values[16];
    UINT texture3d_border_point_values[12];
    UINT texture3d_border_linear_values[12];
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

static void format_hex_array(char *output, size_t output_size,
                             const UINT *values, UINT count)
{
    size_t written = 0;
    UINT i;

    if (!output_size) return;
    output[0] = 0;
    for (i = 0; i < count && written + 1 < output_size; ++i)
    {
        int result = snprintf(output + written, output_size - written,
                              "%s\"0x%08x\"", i ? "," : "", values[i]);
        if (result < 0 || (size_t)result >= output_size - written)
        {
            output[output_size - 1] = 0;
            return;
        }
        written += (size_t)result;
    }
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
    char metrics[24576];
    char texture3d_oob_values[256];
    char texture3d_border_point_values[192];
    char texture3d_border_linear_values[192];
    char heaven_texture3d_r8_values[96];
    char heaven_texture3d_rg8_values[96];
    char heaven_comparison_sampler_values[96];
    char heaven_depth_comparison_sampler_values[96];
    char heaven_d24s8_depth_comparison_sampler_values[96];
    char heaven_d24s8_array_values[128];
    char heaven_d24s8_array_view_values[128];
    char heaven_d24s8_cube_as_array_values[128];
    char heaven_d24s8_cube_sample_values[128];
    char heaven_d24s8_cube_values[128];
    char heaven_d24s8_cube_array_values[256];
    char heaven_d24s8_linear_border_values[96];
    char mrt_gbuffer_values[96];
    char d24_initial_values[96];
    char d24_compare_values[192];
    char d24_rewrite_values[96];
    char rgba16f_initial_values[128];
    char rgba16f_added_values[128];
    char rgba16f_tonemap_values[128];
    char heaven_mini_pipeline_values[96];
    char bc_matrix_values[256];
    const char *version = winehua_smoke_env("WINEHUA_DXVK_VERSION", "unknown");
    BOOL dxvk_loaded;

    dxvk_loaded = dxvk_modules_loaded(state);
    safe_json_text(d3d11_module, sizeof(d3d11_module), state->d3d11_module);
    safe_json_text(dxgi_module, sizeof(dxgi_module), state->dxgi_module);
    format_hex_array(texture3d_oob_values, sizeof(texture3d_oob_values),
                     state->texture3d_oob_values, 16);
    format_hex_array(texture3d_border_point_values,
                     sizeof(texture3d_border_point_values),
                     state->texture3d_border_point_values, 12);
    format_hex_array(texture3d_border_linear_values,
                     sizeof(texture3d_border_linear_values),
                     state->texture3d_border_linear_values, 12);
    format_hex_array(heaven_texture3d_r8_values,
                     sizeof(heaven_texture3d_r8_values),
                     state->heaven_texture3d_r8_values, 4);
    format_hex_array(heaven_texture3d_rg8_values,
                     sizeof(heaven_texture3d_rg8_values),
                     state->heaven_texture3d_rg8_values, 4);
    format_hex_array(heaven_comparison_sampler_values,
                     sizeof(heaven_comparison_sampler_values),
                     state->heaven_comparison_sampler_values, 4);
    format_hex_array(heaven_depth_comparison_sampler_values,
                     sizeof(heaven_depth_comparison_sampler_values),
                     state->heaven_depth_comparison_sampler_values, 4);
    format_hex_array(heaven_d24s8_depth_comparison_sampler_values,
                     sizeof(heaven_d24s8_depth_comparison_sampler_values),
                     state->heaven_d24s8_depth_comparison_sampler_values, 4);
    format_hex_array(heaven_d24s8_array_values,
                     sizeof(heaven_d24s8_array_values),
                     state->heaven_d24s8_array_values,
                     WINEHUA_D24_ARRAY_LAYERS);
    format_hex_array(heaven_d24s8_array_view_values,
                     sizeof(heaven_d24s8_array_view_values),
                     state->heaven_d24s8_array_view_values,
                     WINEHUA_D24_ARRAY_LAYERS);
    format_hex_array(heaven_d24s8_cube_as_array_values,
                     sizeof(heaven_d24s8_cube_as_array_values),
                     state->heaven_d24s8_cube_as_array_values,
                     WINEHUA_D24_ARRAY_LAYERS);
    format_hex_array(heaven_d24s8_cube_sample_values,
                     sizeof(heaven_d24s8_cube_sample_values),
                     state->heaven_d24s8_cube_sample_values,
                     WINEHUA_D24_ARRAY_LAYERS);
    format_hex_array(heaven_d24s8_cube_values,
                     sizeof(heaven_d24s8_cube_values),
                     state->heaven_d24s8_cube_values,
                     WINEHUA_D24_ARRAY_LAYERS);
    format_hex_array(heaven_d24s8_cube_array_values,
                     sizeof(heaven_d24s8_cube_array_values),
                     state->heaven_d24s8_cube_array_values,
                     WINEHUA_D24_CUBE_ARRAY_LAYERS);
    format_hex_array(heaven_d24s8_linear_border_values,
                     sizeof(heaven_d24s8_linear_border_values),
                     state->heaven_d24s8_linear_border_values,
                     WINEHUA_D24_BORDER_CASES);
    format_hex_array(mrt_gbuffer_values, sizeof(mrt_gbuffer_values),
                     state->mrt_gbuffer_values, WINEHUA_MRT_PROBE_POINTS);
    format_hex_array(d24_initial_values, sizeof(d24_initial_values),
                     state->d24_initial_values, WINEHUA_D24_DEPTH_POINTS);
    format_hex_array(d24_compare_values, sizeof(d24_compare_values),
                     state->d24_compare_values, WINEHUA_D24_COMPARE_POINTS);
    format_hex_array(d24_rewrite_values, sizeof(d24_rewrite_values),
                     state->d24_rewrite_values, WINEHUA_D24_DEPTH_POINTS);
    format_hex_array(rgba16f_initial_values, sizeof(rgba16f_initial_values),
                     state->rgba16f_initial_values, WINEHUA_RGBA16F_POINTS);
    format_hex_array(rgba16f_added_values, sizeof(rgba16f_added_values),
                     state->rgba16f_added_values, WINEHUA_RGBA16F_POINTS);
    format_hex_array(rgba16f_tonemap_values, sizeof(rgba16f_tonemap_values),
                     state->rgba16f_tonemap_values, WINEHUA_RGBA16F_POINTS);
    format_hex_array(heaven_mini_pipeline_values,
                     sizeof(heaven_mini_pipeline_values),
                     state->heaven_mini_pipeline_values,
                     WINEHUA_HEAVEN_MINI_POINTS);
    format_hex_array(bc_matrix_values, sizeof(bc_matrix_values),
                     (const UINT *)state->bc_matrix_values,
                     WINEHUA_BC_MATRIX_FORMATS * 2);
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
            "\"bcFormatMipMatrix\":%s,\"bcFormatMatrixValues\":[%s],"
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
             "\"samplerPairMatrix\":{\"shader\":\"t0-t3_shared_s0\","
             "\"values\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],\"pass\":%s},"
             "\"subresourceMatrix\":{\"initialValues\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],"
             "\"updatedValues\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],"
             "\"arrayLayers\":%s,\"mipLevels\":%s,\"explicitLod\":%s,"
             "\"barrierUpdate\":%s,\"pass\":%s},"
             "\"texture3dMatrix\":{\"format\":\"R32G32B32A32_FLOAT\","
             "\"created\":%s,\"upload\":%s,\"singleDispatch\":%s,"
             "\"uavToSrvBarrier\":%s,\"pingPong\":%s,"
             "\"uploadMismatches\":%u,\"singleMismatches\":%u,"
             "\"pingPongMismatches\":%u,"
             "\"oobLoad\":%s,\"oobIndex\":%s,"
             "\"borderPoint\":%s,\"borderLinear\":%s,"
             "\"boundaryMismatches\":[%u,%u,%u,%u],"
             "\"oobValues\":[%s],\"borderPointValues\":[%s],"
             "\"borderLinearValues\":[%s]},"
             "\"heavenResourceMatrix\":{"
             "\"cube\":{\"rgba8\":%s,\"rgba8Srgb\":%s,"
             "\"rgba16Float\":%s,\"bc1\":%s,\"bc1Srgb\":%s,"
             "\"mismatches\":[%u,%u,%u,%u,%u],\"pass\":%s},"
             "\"texture3d\":{"
             "\"r8\":{\"values\":[%s],\"pass\":%s},"
             "\"rg8\":{\"values\":[%s],\"pass\":%s}},"
             "\"comparisonSampler\":{\"values\":[%s],\"pass\":%s},"
             "\"depthComparisonSampler\":{\"values\":[%s],\"pass\":%s},"
             "\"d24s8DepthComparisonSampler\":{\"values\":[%s],\"pass\":%s},"
             "\"d24s8ExtendedMatrix\":{"
             "\"resourceFormat\":\"R24G8_TYPELESS\","
             "\"dsvFormat\":\"D24_UNORM_S8_UINT\","
             "\"srvFormat\":\"R24_UNORM_X8_TYPELESS\","
             "\"array\":{\"values\":[%s],\"mismatches\":%u,\"pass\":%s},"
             "\"arrayViews\":{\"values\":[%s],\"mismatches\":%u,\"pass\":%s},"
             "\"cubeAsArray\":{\"values\":[%s],\"mismatches\":%u,\"pass\":%s},"
             "\"cubeSample\":{\"values\":[%s],\"mismatches\":%u,\"pass\":%s},"
             "\"cube\":{\"values\":[%s],\"mismatches\":%u,\"pass\":%s},"
             "\"cubeArray\":{\"values\":[%s],\"mismatches\":%u,\"pass\":%s},"
             "\"linearBorder\":{\"values\":[%s],\"mismatches\":%u,\"pass\":%s},"
             "\"pass\":%s},"
             "\"pass\":%s},"
             "\"heavenPassMatrix\":{"
             "\"mrtGbuffer\":{\"values\":[%s],\"mismatches\":%u,\"pass\":%s},"
             "\"d24ReadonlyShadow\":{\"initial\":[%s],\"compare\":[%s],"
             "\"rewrite\":[%s],\"mismatches\":%u,\"pass\":%s},"
             "\"rgba16fRtvSrvLoad\":{\"initial\":[%s],\"added\":[%s],"
             "\"tonemap\":[%s],\"mismatches\":%u,\"pass\":%s},"
             "\"miniPipeline\":{\"values\":[%s],\"mismatches\":%u,"
             "\"pass\":%s},"
             "\"pass\":%s},"
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
            state->bc_matrix_functional ? "true" : "false",
            bc_matrix_values,
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
             state->sampler_pair_values[0],
             state->sampler_pair_values[1],
             state->sampler_pair_values[2],
             state->sampler_pair_values[3],
             state->sampler_pair_functional ? "true" : "false",
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
             state->texture3d_created ? "true" : "false",
             state->texture3d_upload_functional ? "true" : "false",
             state->texture3d_single_dispatch_functional ? "true" : "false",
             state->texture3d_uav_to_srv_functional ? "true" : "false",
             state->texture3d_pingpong_functional ? "true" : "false",
             state->texture3d_upload_mismatches,
             state->texture3d_single_mismatches,
             state->texture3d_pingpong_mismatches,
             state->texture3d_oob_load_functional ? "true" : "false",
             state->texture3d_oob_index_functional ? "true" : "false",
             state->texture3d_border_point_functional ? "true" : "false",
             state->texture3d_border_linear_functional ? "true" : "false",
             state->texture3d_oob_load_mismatches,
             state->texture3d_oob_index_mismatches,
             state->texture3d_border_point_mismatches,
             state->texture3d_border_linear_mismatches,
             texture3d_oob_values,
             texture3d_border_point_values,
             texture3d_border_linear_values,
             state->heaven_cube_cases[0] ? "true" : "false",
             state->heaven_cube_cases[1] ? "true" : "false",
             state->heaven_cube_cases[2] ? "true" : "false",
             state->heaven_cube_cases[3] ? "true" : "false",
             state->heaven_cube_cases[4] ? "true" : "false",
             state->heaven_cube_mismatches[0],
             state->heaven_cube_mismatches[1],
             state->heaven_cube_mismatches[2],
             state->heaven_cube_mismatches[3],
             state->heaven_cube_mismatches[4],
             state->heaven_cube_functional ? "true" : "false",
             heaven_texture3d_r8_values,
             state->heaven_texture3d_r8_functional ? "true" : "false",
             heaven_texture3d_rg8_values,
             state->heaven_texture3d_rg8_functional ? "true" : "false",
             heaven_comparison_sampler_values,
             state->heaven_comparison_sampler_functional ? "true" : "false",
             heaven_depth_comparison_sampler_values,
             state->heaven_depth_comparison_sampler_functional ? "true" : "false",
             heaven_d24s8_depth_comparison_sampler_values,
             state->heaven_d24s8_depth_comparison_sampler_functional ? "true" : "false",
             heaven_d24s8_array_values,
             state->heaven_d24s8_extended_mismatches[0],
             state->heaven_d24s8_array_functional ? "true" : "false",
             heaven_d24s8_array_view_values,
             state->heaven_d24s8_extended_mismatches[1],
             state->heaven_d24s8_array_views_functional ? "true" : "false",
             heaven_d24s8_cube_as_array_values,
             state->heaven_d24s8_extended_mismatches[2],
             state->heaven_d24s8_cube_as_array_functional ? "true" : "false",
             heaven_d24s8_cube_sample_values,
             state->heaven_d24s8_extended_mismatches[3],
             state->heaven_d24s8_cube_sample_functional ? "true" : "false",
             heaven_d24s8_cube_values,
             state->heaven_d24s8_extended_mismatches[4],
             state->heaven_d24s8_cube_functional ? "true" : "false",
             heaven_d24s8_cube_array_values,
             state->heaven_d24s8_extended_mismatches[5],
             state->heaven_d24s8_cube_array_functional ? "true" : "false",
             heaven_d24s8_linear_border_values,
             state->heaven_d24s8_extended_mismatches[6],
             state->heaven_d24s8_linear_border_functional ? "true" : "false",
             state->heaven_d24s8_extended_functional ? "true" : "false",
             state->heaven_resource_functional ? "true" : "false",
             mrt_gbuffer_values,
             state->mrt_gbuffer_mismatches,
             state->mrt_gbuffer_functional ? "true" : "false",
             d24_initial_values, d24_compare_values, d24_rewrite_values,
             state->d24_readonly_shadow_mismatches,
             state->d24_readonly_shadow_functional ? "true" : "false",
             rgba16f_initial_values, rgba16f_added_values,
             rgba16f_tonemap_values,
             state->rgba16f_rtv_srv_load_mismatches,
             state->rgba16f_rtv_srv_load_functional ? "true" : "false",
             heaven_mini_pipeline_values,
             state->heaven_mini_pipeline_mismatches,
             state->heaven_mini_pipeline_functional ? "true" : "false",
             state->heaven_pass_functional ? "true" : "false",
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

static ID3D11PixelShader *create_probe_pixel_shader(
    struct smoke_state *state, const char *source, const char *name)
{
    ID3DBlob *blob = NULL;
    ID3D11PixelShader *shader = NULL;
    HRESULT result;

    if (!compile_shader(source, "main", "ps_5_0", &blob))
        return NULL;
    result = ID3D11Device_CreatePixelShader(
        state->device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &shader);
    ID3D10Blob_Release(blob);
    if (FAILED(result))
    {
        fprintf(stderr,
                "winehua_d3d11_smoke: Heaven %s pixel shader failed=0x%08lx\n",
                name, (unsigned long)result);
        return NULL;
    }
    return shader;
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

static BOOL create_bc_matrix_resources(struct smoke_state *state)
{
    static const unsigned char bc1_red[8] = {
        0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc1_green[8] = {
        0xe0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc1_blue[8] = {
        0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc3_red[16] = {
        0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc3_green[16] = {
        0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xe0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc3_blue[16] = {
        0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc4_red[8] = {
        0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc4_half_red[8] = {
        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc5_yellow[16] = {
        0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc5_green[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc4_snorm_neg[8] = {
        0x81, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc4_snorm_zero[8] = {
        0x00, 0x7f, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24
    };
    static const unsigned char bc5_snorm_neg[16] = {
        0x81, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x81, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc5_snorm_zero_pos[16] = {
        0x00, 0x7f, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24,
        0x7f, 0x00, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24
    };
    static const struct
    {
        DXGI_FORMAT format;
        const unsigned char *mip0;
        const unsigned char *mip1;
        UINT block_size;
    } cases[WINEHUA_BC_MATRIX_FORMATS] = {
        {DXGI_FORMAT_BC1_UNORM,      bc1_red,    bc1_green, 8},
        {DXGI_FORMAT_BC1_UNORM_SRGB, bc1_red,    bc1_blue,  8},
        {DXGI_FORMAT_BC3_UNORM,      bc3_red,    bc3_blue, 16},
        {DXGI_FORMAT_BC3_UNORM_SRGB, bc3_red,    bc3_green,16},
        {DXGI_FORMAT_BC4_UNORM,      bc4_red,    bc4_half_red, 8},
        {DXGI_FORMAT_BC5_UNORM,      bc5_yellow, bc5_green,16},
        {DXGI_FORMAT_BC4_SNORM,      bc4_snorm_neg, bc4_snorm_zero, 8},
        {DXGI_FORMAT_BC5_SNORM,      bc5_snorm_neg, bc5_snorm_zero_pos,16},
    };
    D3D11_SAMPLER_DESC sampler_desc = {0};
    UINT i;
    HRESULT result = S_OK;

    for (i = 0; i < WINEHUA_BC_MATRIX_FORMATS; ++i)
    {
        D3D11_TEXTURE2D_DESC desc = {0};
        D3D11_SUBRESOURCE_DATA data[2] = {{0}};
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};

        desc.Width = 4;
        desc.Height = 4;
        desc.MipLevels = 2;
        desc.ArraySize = 1;
        desc.Format = cases[i].format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        data[0].pSysMem = cases[i].mip0;
        data[0].SysMemPitch = cases[i].block_size;
        data[0].SysMemSlicePitch = cases[i].block_size;
        data[1].pSysMem = cases[i].mip1;
        data[1].SysMemPitch = cases[i].block_size;
        data[1].SysMemSlicePitch = cases[i].block_size;

        result = ID3D11Device_CreateTexture2D(
            state->device, &desc, data, &state->bc_matrix_textures[i]);
        if (FAILED(result))
        {
            fprintf(stderr,
                    "winehua_d3d11_smoke: BC matrix texture %u format=%u failed=0x%08lx\n",
                    i, (unsigned int)cases[i].format, (unsigned long)result);
            break;
        }

        srv_desc.Format = cases[i].format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = 2;
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)state->bc_matrix_textures[i],
            &srv_desc, &state->bc_matrix_srvs[i]);
        if (FAILED(result))
        {
            fprintf(stderr,
                    "winehua_d3d11_smoke: BC matrix SRV %u format=%u failed=0x%08lx\n",
                    i, (unsigned int)cases[i].format, (unsigned long)result);
            break;
        }
    }

    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 1.0f;
    sampler_desc.MaxLOD = 1.0f;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateSamplerState(
            state->device, &sampler_desc, &state->bc_mip_sampler);
    if (FAILED(result))
    {
        state->present_result = result;
        return FALSE;
    }
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
    const char *sampler_pair_source =
        "Texture2D tex0 : register(t0);"
        "Texture2D tex1 : register(t1);"
        "Texture2D tex2 : register(t2);"
        "Texture2D tex3 : register(t3);"
        "SamplerState sharedSampler : register(s0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "int x = input.pos.x >= 32.0; int y = input.pos.y >= 32.0;"
        "int slot = x + (y << 1); float2 uv = float2(0.5, 0.5);"
        "if (slot == 0) return tex0.SampleLevel(sharedSampler, uv, 0.0);"
        "if (slot == 1) return tex1.SampleLevel(sharedSampler, uv, 0.0);"
        "if (slot == 2) return tex2.SampleLevel(sharedSampler, uv, 0.0);"
        "return tex3.SampleLevel(sharedSampler, uv, 0.0); }";
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
    const char *texture3d_pingpong_source =
        "Texture3D<float4> inputTexture : register(t0);"
        "SamplerState inputSampler : register(s0);"
        "RWTexture3D<float4> outputTexture : register(u0);"
        "[numthreads(2, 2, 2)]"
        "void main(uint3 id : SV_DispatchThreadID) {"
        "float3 uv = (float3(id) + 0.5) / float3(4.0, 4.0, 4.0);"
        "outputTexture[id] = inputTexture.SampleLevel(inputSampler, uv, 0.0)"
        "+ float4(1.0, 2.0, 4.0, 8.0); }";
    const char *texture3d_oob_source =
        "Texture3D<float4> inputTexture : register(t0);"
        "RWTexture2D<float4> outputTexture : register(u0);"
        "[numthreads(16, 1, 1)]"
        "void main(uint3 id : SV_DispatchThreadID) {"
        "uint c = id.x & 7; int3 p = int3(0, 0, 0);"
        "if (c == 0) p = int3(-1, 0, 0);"
        "if (c == 1) p = int3(4, 0, 0);"
        "if (c == 2) p = int3(0, -1, 0);"
        "if (c == 3) p = int3(0, 4, 0);"
        "if (c == 4) p = int3(0, 0, -1);"
        "if (c == 5) p = int3(0, 0, 4);"
        "if (c == 6) p = int3(-1, -1, -1);"
        "if (c == 7) p = int3(4, 4, 4);"
        "float4 value = id.x < 8 ? inputTexture.Load(int4(p, 0))"
        " : inputTexture[(uint3)p];"
        "outputTexture[uint2(id.x, 0)] = value;"
        "if (id.x == 0) outputTexture[uint2(0, 1)] = float4(0, 1, 1, 1); }";
    const char *texture3d_border_source =
        "Texture3D<float4> inputTexture : register(t0);"
        "SamplerState inputSampler : register(s0);"
        "RWTexture2D<float4> outputTexture : register(u0);"
        "[numthreads(12, 1, 1)]"
        "void main(uint3 id : SV_DispatchThreadID) {"
        "float3 p = float3(0.5, 0.5, 0.5);"
        "if (id.x == 0) p = float3(-0.25, 0.5, 0.5);"
        "if (id.x == 1) p = float3(1.25, 0.5, 0.5);"
        "if (id.x == 2) p = float3(0.5, -0.25, 0.5);"
        "if (id.x == 3) p = float3(0.5, 1.25, 0.5);"
        "if (id.x == 4) p = float3(0.5, 0.5, -0.25);"
        "if (id.x == 5) p = float3(0.5, 0.5, 1.25);"
        "if (id.x == 7) p = float3(0.0, 0.0, 0.0);"
        "if (id.x == 8) p = float3(0.0, 0.5, 0.5);"
        "if (id.x == 9) p = float3(1.0, 0.5, 0.5);"
        "if (id.x == 10) p = float3(0.0, 0.0, 0.5);"
        "if (id.x == 11) p = float3(1.0, 1.0, 0.5);"
        "outputTexture[uint2(id.x, 0)] ="
        " inputTexture.SampleLevel(inputSampler, p, 0.0);"
        "if (id.x == 0) outputTexture[uint2(0, 1)] = float4(1, 0, 1, 1); }";
    ID3DBlob *fullscreen_blob = NULL, *probe_blob = NULL;
    ID3DBlob *overlay_blob = NULL, *compute_blob = NULL;
    ID3DBlob *rgba_sample_blob = NULL, *rgba_load_blob = NULL;
    ID3DBlob *descriptor_identity_blob = NULL, *sampler_pair_blob = NULL;
    ID3DBlob *subresource_blob = NULL;
    ID3DBlob *rgba_sample_compute_blob = NULL, *rgba_load_compute_blob = NULL;
    ID3DBlob *texture3d_pingpong_blob = NULL;
    ID3DBlob *texture3d_oob_blob = NULL, *texture3d_border_blob = NULL;
    HRESULT result = E_FAIL;

    if (!compile_shader(fullscreen_source, "main", "vs_5_0", &fullscreen_blob) ||
        !compile_shader(bc_probe_source, "main", "ps_5_0", &probe_blob) ||
        !compile_shader(rgba_sample_source, "main", "ps_5_0", &rgba_sample_blob) ||
        !compile_shader(rgba_load_source, "main", "ps_5_0", &rgba_load_blob) ||
        !compile_shader(descriptor_identity_source, "main", "ps_5_0", &descriptor_identity_blob) ||
        !compile_shader(sampler_pair_source, "main", "ps_5_0", &sampler_pair_blob) ||
        !compile_shader(subresource_source, "main", "ps_5_0", &subresource_blob) ||
        !compile_shader(stencil_overlay_source, "main", "ps_5_0", &overlay_blob) ||
        !compile_shader(compute_source, "main", "cs_5_0", &compute_blob) ||
        !compile_shader(rgba_sample_compute_source, "main", "cs_5_0", &rgba_sample_compute_blob) ||
        !compile_shader(rgba_load_compute_source, "main", "cs_5_0", &rgba_load_compute_blob) ||
        !compile_shader(texture3d_pingpong_source, "main", "cs_5_0",
                        &texture3d_pingpong_blob) ||
        !compile_shader(texture3d_oob_source, "main", "cs_5_0",
                        &texture3d_oob_blob) ||
        !compile_shader(texture3d_border_source, "main", "cs_5_0",
                        &texture3d_border_blob))
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
            state->device, ID3D10Blob_GetBufferPointer(sampler_pair_blob),
            ID3D10Blob_GetBufferSize(sampler_pair_blob), NULL,
            &state->sampler_pair_pixel_shader);
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
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateComputeShader(
            state->device, ID3D10Blob_GetBufferPointer(texture3d_pingpong_blob),
            ID3D10Blob_GetBufferSize(texture3d_pingpong_blob), NULL,
            &state->texture3d_pingpong_compute_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateComputeShader(
            state->device, ID3D10Blob_GetBufferPointer(texture3d_oob_blob),
            ID3D10Blob_GetBufferSize(texture3d_oob_blob), NULL,
            &state->texture3d_oob_compute_shader);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateComputeShader(
            state->device, ID3D10Blob_GetBufferPointer(texture3d_border_blob),
            ID3D10Blob_GetBufferSize(texture3d_border_blob), NULL,
            &state->texture3d_border_compute_shader);

done:
    if (fullscreen_blob) ID3D10Blob_Release(fullscreen_blob);
    if (probe_blob) ID3D10Blob_Release(probe_blob);
    if (overlay_blob) ID3D10Blob_Release(overlay_blob);
    if (compute_blob) ID3D10Blob_Release(compute_blob);
    if (rgba_sample_blob) ID3D10Blob_Release(rgba_sample_blob);
    if (rgba_load_blob) ID3D10Blob_Release(rgba_load_blob);
    if (descriptor_identity_blob) ID3D10Blob_Release(descriptor_identity_blob);
    if (sampler_pair_blob) ID3D10Blob_Release(sampler_pair_blob);
    if (subresource_blob) ID3D10Blob_Release(subresource_blob);
    if (rgba_sample_compute_blob) ID3D10Blob_Release(rgba_sample_compute_blob);
    if (rgba_load_compute_blob) ID3D10Blob_Release(rgba_load_compute_blob);
    if (texture3d_pingpong_blob) ID3D10Blob_Release(texture3d_pingpong_blob);
    if (texture3d_oob_blob) ID3D10Blob_Release(texture3d_oob_blob);
    if (texture3d_border_blob) ID3D10Blob_Release(texture3d_border_blob);
    if (FAILED(result))
    {
        state->present_result = result;
        return FALSE;
    }
    return TRUE;
}

static void fill_texture3d_initial(float values[WINEHUA_TEXTURE3D_TEXELS][4])
{
    UINT i;
    for (i = 0; i < WINEHUA_TEXTURE3D_TEXELS; ++i)
    {
        values[i][0] = (float)i;
        values[i][1] = (float)(i + 64);
        values[i][2] = (float)(i + 128);
        values[i][3] = (float)(i + 192);
    }
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

    if (SUCCEEDED(result))
    {
        D3D11_TEXTURE3D_DESC texture_desc = {0};
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {0};
        D3D11_SAMPLER_DESC sampler_desc = {0};
        D3D11_SUBRESOURCE_DATA initial_data = {0};
        D3D11_SUBRESOURCE_DATA border_data = {0};
        float initial_values[WINEHUA_TEXTURE3D_TEXELS][4];
        float border_values[WINEHUA_TEXTURE3D_TEXELS][4];
        UINT i;

        fill_texture3d_initial(initial_values);
        for (i = 0; i < WINEHUA_TEXTURE3D_TEXELS; ++i)
            border_values[i][0] = border_values[i][1] =
            border_values[i][2] = border_values[i][3] = 0.25f;
        texture_desc.Width = WINEHUA_TEXTURE3D_SIZE;
        texture_desc.Height = WINEHUA_TEXTURE3D_SIZE;
        texture_desc.Depth = WINEHUA_TEXTURE3D_SIZE;
        texture_desc.MipLevels = 1;
        texture_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                                 D3D11_BIND_UNORDERED_ACCESS;
        initial_data.pSysMem = initial_values;
        initial_data.SysMemPitch = WINEHUA_TEXTURE3D_SIZE * 4 * sizeof(float);
        initial_data.SysMemSlicePitch = WINEHUA_TEXTURE3D_SIZE *
                                        initial_data.SysMemPitch;

        result = ID3D11Device_CreateTexture3D(
            state->device, &texture_desc, &initial_data, &state->texture3d[0]);
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateTexture3D(
                state->device, &texture_desc, NULL, &state->texture3d[1]);

        srv_desc.Format = texture_desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        srv_desc.Texture3D.MostDetailedMip = 0;
        srv_desc.Texture3D.MipLevels = 1;
        uav_desc.Format = texture_desc.Format;
        uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
        uav_desc.Texture3D.FirstWSlice = 0;
        uav_desc.Texture3D.WSize = WINEHUA_TEXTURE3D_SIZE;
        for (i = 0; SUCCEEDED(result) && i < 2; ++i)
        {
            result = ID3D11Device_CreateShaderResourceView(
                state->device, (ID3D11Resource *)state->texture3d[i],
                &srv_desc, &state->texture3d_srvs[i]);
            if (SUCCEEDED(result))
                result = ID3D11Device_CreateUnorderedAccessView(
                    state->device, (ID3D11Resource *)state->texture3d[i],
                    &uav_desc, &state->texture3d_uavs[i]);
        }

        texture_desc.Usage = D3D11_USAGE_STAGING;
        texture_desc.BindFlags = 0;
        texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateTexture3D(
                state->device, &texture_desc, NULL, &state->texture3d_staging);

        texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
        texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        texture_desc.CPUAccessFlags = 0;
        border_data.pSysMem = border_values;
        border_data.SysMemPitch = WINEHUA_TEXTURE3D_SIZE * 4 * sizeof(float);
        border_data.SysMemSlicePitch = WINEHUA_TEXTURE3D_SIZE *
                                       border_data.SysMemPitch;
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateTexture3D(
                state->device, &texture_desc, &border_data,
                &state->texture3d_border);
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateShaderResourceView(
                state->device, (ID3D11Resource *)state->texture3d_border,
                &srv_desc, &state->texture3d_border_srv);

        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        sampler_desc.MipLODBias = 0.0f;
        sampler_desc.MaxAnisotropy = 1;
        sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler_desc.BorderColor[0] = 0.5f;
        sampler_desc.BorderColor[1] = 0.5f;
        sampler_desc.BorderColor[2] = 0.5f;
        sampler_desc.BorderColor[3] = 0.5f;
        sampler_desc.MinLOD = 0.0f;
        sampler_desc.MaxLOD = 0.0f;
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateSamplerState(
                state->device, &sampler_desc, &state->border_point_sampler);
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateSamplerState(
                state->device, &sampler_desc, &state->border_linear_sampler);
        if (SUCCEEDED(result)) state->texture3d_created = TRUE;
    }

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

static BOOL run_texture_ps_probe(struct smoke_state *state,
                                 ID3D11ShaderResourceView *resource,
                                 ID3D11PixelShader *shader,
                                 ID3D11SamplerState *sampler,
                                 const char *name, UINT expected,
                                 UINT *sampled_value)
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
        fprintf(stderr, "winehua_d3d11_smoke: texture %s PS map failed=0x%08lx\\n",
                name, (unsigned long)result);
        return FALSE;
    }
    *sampled_value = rgba8_value(&mapped, 32, 32);
    ID3D11DeviceContext_Unmap(
        state->context, (ID3D11Resource *)state->probe_staging, 0);
    state->feature_probe_read_bytes += WINEHUA_FEATURE_PROBE_SIZE *
                                       WINEHUA_FEATURE_PROBE_SIZE * 4;
    fprintf(stderr, "winehua_d3d11_smoke: texture %s PS value=0x%08x expected=0x%08x\\n",
            name, *sampled_value, expected);
    return *sampled_value == expected;
}

static BOOL run_bc_matrix_probes(struct smoke_state *state)
{
    static const char *names[WINEHUA_BC_MATRIX_FORMATS] = {
        "BC1_UNORM", "BC1_SRGB", "BC3_UNORM",
        "BC3_SRGB", "BC4_UNORM", "BC5_UNORM",
        "BC4_SNORM", "BC5_SNORM",
    };
    static const UINT mip0_expected[WINEHUA_BC_MATRIX_FORMATS] = {
        0xff0000ffu, 0xff0000ffu, 0xff0000ffu,
        0xff0000ffu, 0xff0000ffu, 0xff00ffffu,
        0xff000000u, 0xff000000u,
    };
    static const UINT mip1_expected[WINEHUA_BC_MATRIX_FORMATS] = {
        0xff00ff00u, 0xffff0000u, 0xffff0000u,
        0xff00ff00u, 0xff000080u, 0xff00ff00u,
        0xff0000ffu, 0xff0000ffu,
    };
    char label[64];
    BOOL pass = TRUE;
    UINT i;

    for (i = 0; i < WINEHUA_BC_MATRIX_FORMATS; ++i)
    {
        snprintf(label, sizeof(label), "%s mip0", names[i]);
        if (!run_texture_ps_probe(
                state, state->bc_matrix_srvs[i],
                state->rgba_sample_pixel_shader, state->bc_sampler,
                label, mip0_expected[i], &state->bc_matrix_values[i][0]))
            pass = FALSE;

        snprintf(label, sizeof(label), "%s mip1", names[i]);
        if (!run_texture_ps_probe(
                state, state->bc_matrix_srvs[i],
                state->rgba_sample_pixel_shader, state->bc_mip_sampler,
                label, mip1_expected[i], &state->bc_matrix_values[i][1]))
            pass = FALSE;
    }

    state->bc_matrix_functional = pass;
    return pass;
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
    ID3D11ShaderResourceView *release_srv,
    ID3D11PixelShader *pixel_shader,
    ID3D11SamplerState *sampler)
{
    static const float marker[4] = {
        239.0f / 255.0f, 190.0f / 255.0f,
        173.0f / 255.0f, 222.0f / 255.0f,
    };
    D3D11_VIEWPORT viewport = {0, 0, WINEHUA_FEATURE_PROBE_SIZE,
                               WINEHUA_FEATURE_PROBE_SIZE, 0.0f, 1.0f};
    ID3D11RenderTargetView *targets[] = {state->probe_rtv};
    ID3D11ShaderResourceView *null_resources[] = {NULL, NULL, NULL, NULL};
    ID3D11SamplerState *samplers[] = {sampler};
    ID3D11SamplerState *null_samplers[] = {NULL};
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
        state->context, pixel_shader ? pixel_shader
                                     : state->descriptor_identity_pixel_shader,
        NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(state->context, 0, 4, resources);
    if (sampler)
        ID3D11DeviceContext_PSSetSamplers(state->context, 0, 1, samplers);

    /* Drop the application's final references after binding.  The immediate
     * context must retain the SRV/resource through the draw and descriptor
     * update, which makes this a real lifetime test rather than a non-NULL
     * handle check. */
    if (release_srv) ID3D11ShaderResourceView_Release(release_srv);
    if (release_texture) ID3D11Texture2D_Release(release_texture);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(
        state->context, 0, 4, null_resources);
    if (sampler)
        ID3D11DeviceContext_PSSetSamplers(
            state->context, 0, 1, null_samplers);
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
        "initial", NULL, NULL, NULL, NULL);
    state->descriptor_rebind_functional = run_descriptor_identity_pass(
        state, rebind, rebind[0] == state->descriptor_srvs[3] ?
        (const UINT[]){0xff00ffffu, 0xffff0000u, 0xff00ff00u, 0xff0000ffu} : colors,
        state->descriptor_rebind_values, "rebind", NULL, NULL, NULL, NULL);
    state->descriptor_unbound_functional = run_descriptor_identity_pass(
        state, unbound, unbound_expected, state->descriptor_unbound_values,
        "unbound", NULL, NULL, NULL, NULL);

    if (!create_solid_rgba_texture(state, 0xffff00ffu,
                                   &lifetime_texture, &lifetime_srv))
        return FALSE;
    lifetime[0] = state->descriptor_srvs[0];
    lifetime[1] = state->descriptor_srvs[1];
    lifetime[2] = lifetime_srv;
    lifetime[3] = state->descriptor_srvs[3];
    state->descriptor_lifetime_functional = run_descriptor_identity_pass(
        state, lifetime, lifetime_expected, state->descriptor_lifetime_values,
        "lifetime", lifetime_texture, lifetime_srv, NULL, NULL);
    return state->descriptor_identity_functional &&
           state->descriptor_rebind_functional &&
           state->descriptor_unbound_functional &&
           state->descriptor_lifetime_functional;
}

static BOOL run_sampler_pair_probe(struct smoke_state *state)
{
    static const UINT expected[4] = {
        0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff00ffffu,
    };

    state->sampler_pair_functional = run_descriptor_identity_pass(
        state, state->descriptor_srvs, expected, state->sampler_pair_values,
        "shared-s0", NULL, NULL, state->sampler_pair_pixel_shader,
        state->point_sampler);
    return state->sampler_pair_functional;
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

static BOOL validate_texture3d(struct smoke_state *state,
                               ID3D11Texture3D *texture,
                               UINT dispatch_count, UINT *mismatch_count,
                               const char *name)
{
    static const float delta[4] = {1.0f, 2.0f, 4.0f, 8.0f};
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT result;
    UINT x, y, z, component, mismatches = 0;
    float first_actual[4] = {0}, first_expected[4] = {0};

    ID3D11DeviceContext_CopyResource(
        state->context, (ID3D11Resource *)state->texture3d_staging,
        (ID3D11Resource *)texture);
    state->feature_probe_gpu_copies++;
    ID3D11DeviceContext_Flush(state->context);
    result = ID3D11DeviceContext_Map(
        state->context, (ID3D11Resource *)state->texture3d_staging, 0,
        D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        *mismatch_count = ~0u;
        fprintf(stderr,
                "winehua_d3d11_smoke: Texture3D %s map failed=0x%08lx\n",
                name, (unsigned long)result);
        return FALSE;
    }

    for (z = 0; z < WINEHUA_TEXTURE3D_SIZE; ++z)
    {
        for (y = 0; y < WINEHUA_TEXTURE3D_SIZE; ++y)
        {
            for (x = 0; x < WINEHUA_TEXTURE3D_SIZE; ++x)
            {
                UINT index = (z * WINEHUA_TEXTURE3D_SIZE + y) *
                             WINEHUA_TEXTURE3D_SIZE + x;
                const float *actual = (const float *)((const BYTE *)mapped.pData +
                    z * mapped.DepthPitch + y * mapped.RowPitch +
                    x * 4 * sizeof(float));
                const float initial[4] = {
                    (float)index, (float)(index + 64),
                    (float)(index + 128), (float)(index + 192)
                };
                for (component = 0; component < 4; ++component)
                {
                    float expected = initial[component] +
                                     dispatch_count * delta[component];
                    float difference = actual[component] - expected;
                    if (index == 0)
                    {
                        first_actual[component] = actual[component];
                        first_expected[component] = expected;
                    }
                    if (difference < -0.001f || difference > 0.001f)
                        ++mismatches;
                }
            }
        }
    }
    ID3D11DeviceContext_Unmap(
        state->context, (ID3D11Resource *)state->texture3d_staging, 0);
    state->feature_probe_read_bytes += WINEHUA_TEXTURE3D_TEXELS *
                                       4 * sizeof(float);
    *mismatch_count = mismatches;
    fprintf(stderr,
            "winehua_d3d11_smoke: Texture3D %s first="
            "%.1f,%.1f,%.1f,%.1f expected=%.1f,%.1f,%.1f,%.1f mismatches=%u\n",
            name, first_actual[0], first_actual[1], first_actual[2],
            first_actual[3], first_expected[0], first_expected[1],
            first_expected[2], first_expected[3], mismatches);
    return mismatches == 0;
}

static void dispatch_texture3d(struct smoke_state *state,
                               ID3D11ShaderResourceView *source,
                               ID3D11UnorderedAccessView *target)
{
    ID3D11ShaderResourceView *resources[] = {source};
    ID3D11ShaderResourceView *null_resources[] = {NULL};
    ID3D11UnorderedAccessView *uavs[] = {target};
    ID3D11UnorderedAccessView *null_uavs[] = {NULL};
    ID3D11SamplerState *samplers[] = {state->linear_sampler};
    ID3D11SamplerState *null_samplers[] = {NULL};

    ID3D11DeviceContext_CSSetShader(
        state->context, state->texture3d_pingpong_compute_shader, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(state->context, 0, 1, resources);
    ID3D11DeviceContext_CSSetSamplers(state->context, 0, 1, samplers);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(
        state->context, 0, 1, uavs, NULL);
    ID3D11DeviceContext_Dispatch(state->context, 2, 2, 2);
    ID3D11DeviceContext_CSSetShaderResources(
        state->context, 0, 1, null_resources);
    ID3D11DeviceContext_CSSetSamplers(
        state->context, 0, 1, null_samplers);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(
        state->context, 0, 1, null_uavs, NULL);
}

static BOOL run_texture3d_probes(struct smoke_state *state)
{
    float initial_values[WINEHUA_TEXTURE3D_TEXELS][4];
    const UINT row_pitch = WINEHUA_TEXTURE3D_SIZE * 4 * sizeof(float);
    const UINT slice_pitch = WINEHUA_TEXTURE3D_SIZE * row_pitch;

    if (!state->texture3d_created) return FALSE;
    fill_texture3d_initial(initial_values);
    state->texture3d_upload_functional = validate_texture3d(
        state, state->texture3d[0], 0, &state->texture3d_upload_mismatches,
        "upload");

    dispatch_texture3d(
        state, state->texture3d_srvs[0], state->texture3d_uavs[1]);
    state->texture3d_single_dispatch_functional = validate_texture3d(
        state, state->texture3d[1], 1,
        &state->texture3d_single_mismatches, "single-dispatch");

    /* Reset the source, then issue the two passes without Flush/Map or a CPU
     * wait in between. This matches ComputeMark's Texture3D/RWTexture3D
     * ping-pong contract and makes the UAV-write -> SRV-read dependency the
     * only new ordering requirement in the second dispatch. */
    ID3D11DeviceContext_UpdateSubresource(
        state->context, (ID3D11Resource *)state->texture3d[0], 0, NULL,
        initial_values, row_pitch, slice_pitch);
    state->feature_probe_gpu_copies++;
    dispatch_texture3d(
        state, state->texture3d_srvs[0], state->texture3d_uavs[1]);
    dispatch_texture3d(
        state, state->texture3d_srvs[1], state->texture3d_uavs[0]);
    ID3D11DeviceContext_CSSetShader(state->context, NULL, NULL, 0);

    state->texture3d_pingpong_functional = validate_texture3d(
        state, state->texture3d[0], 2,
        &state->texture3d_pingpong_mismatches, "ping-pong");
    state->texture3d_uav_to_srv_functional =
        state->texture3d_single_dispatch_functional &&
        state->texture3d_pingpong_functional;
    return state->texture3d_upload_functional &&
           state->texture3d_single_dispatch_functional &&
           state->texture3d_uav_to_srv_functional &&
           state->texture3d_pingpong_functional;
}

static BOOL gray_value_matches(UINT value, BYTE expected, BYTE tolerance)
{
    UINT component;
    for (component = 0; component < 4; ++component)
    {
        BYTE actual = (BYTE)(value >> (component * 8));
        int difference = (int)actual - (int)expected;
        if (difference < -(int)tolerance || difference > (int)tolerance)
            return FALSE;
    }
    return TRUE;
}

static BOOL packed_value_matches(UINT value, UINT expected, BYTE tolerance)
{
    UINT component;
    for (component = 0; component < 4; ++component)
    {
        BYTE actual_component = (BYTE)(value >> (component * 8));
        BYTE expected_component = (BYTE)(expected >> (component * 8));
        int difference = (int)actual_component - (int)expected_component;
        if (difference < -(int)tolerance || difference > (int)tolerance)
            return FALSE;
    }
    return TRUE;
}

static BOOL run_texture3d_output_probe(
    struct smoke_state *state,
    ID3D11ComputeShader *shader,
    ID3D11ShaderResourceView *resource,
    ID3D11SamplerState *sampler,
    UINT output_count,
    UINT marker_expected,
    UINT *values,
    const char *name)
{
    static const float clear_value[4] = {
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
    HRESULT result;
    UINT marker, i;

    ID3D11DeviceContext_ClearUnorderedAccessViewFloat(
        state->context, state->compute_uav, clear_value);
    ID3D11DeviceContext_CSSetShader(state->context, shader, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(state->context, 0, 1, resources);
    ID3D11DeviceContext_CSSetSamplers(state->context, 0, 1, samplers);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(
        state->context, 0, 1, uavs, NULL);
    ID3D11DeviceContext_Dispatch(state->context, 1, 1, 1);
    ID3D11DeviceContext_CSSetShaderResources(
        state->context, 0, 1, null_resources);
    ID3D11DeviceContext_CSSetSamplers(
        state->context, 0, 1, null_samplers);
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
    if (FAILED(result))
    {
        fprintf(stderr,
                "winehua_d3d11_smoke: Texture3D %s map failed=0x%08lx\n",
                name, (unsigned long)result);
        return FALSE;
    }
    for (i = 0; i < output_count; ++i)
        values[i] = rgba8_value(&mapped, i, 0);
    marker = rgba8_value(&mapped, 0, 1);
    ID3D11DeviceContext_Unmap(
        state->context, (ID3D11Resource *)state->compute_staging, 0);
    state->feature_probe_read_bytes += 16 * 16 * 4;

    fprintf(stderr, "winehua_d3d11_smoke: Texture3D %s values=", name);
    for (i = 0; i < output_count; ++i)
        fprintf(stderr, "%s0x%08x", i ? "," : "", values[i]);
    fprintf(stderr, " marker=0x%08x expected=0x%08x\n",
            marker, marker_expected);
    return marker == marker_expected;
}

static BOOL run_texture3d_boundary_probes(struct smoke_state *state)
{
    static const BYTE linear_expected[12] = {
        128, 128, 128, 128, 128, 128,
        64, 120, 96, 96, 112, 112,
    };
    UINT i;
    BOOL marker_ok;

    marker_ok = run_texture3d_output_probe(
        state, state->texture3d_oob_compute_shader,
        state->texture3d_srvs[0], NULL, 16, 0xffffff00u,
        state->texture3d_oob_values, "OOB Load/index");
    for (i = 0; i < 8; ++i)
        if (state->texture3d_oob_values[i] != 0)
            ++state->texture3d_oob_load_mismatches;
    for (i = 8; i < 16; ++i)
        if (state->texture3d_oob_values[i] != 0)
            ++state->texture3d_oob_index_mismatches;
    state->texture3d_oob_load_functional = marker_ok &&
        state->texture3d_oob_load_mismatches == 0;
    state->texture3d_oob_index_functional = marker_ok &&
        state->texture3d_oob_index_mismatches == 0;

    marker_ok = run_texture3d_output_probe(
        state, state->texture3d_border_compute_shader,
        state->texture3d_border_srv, state->border_point_sampler,
        12, 0xffff00ffu, state->texture3d_border_point_values,
        "BorderColor point");
    for (i = 0; i < 6; ++i)
        if (!gray_value_matches(state->texture3d_border_point_values[i], 128, 2))
            ++state->texture3d_border_point_mismatches;
    if (!gray_value_matches(state->texture3d_border_point_values[6], 64, 2))
        ++state->texture3d_border_point_mismatches;
    state->texture3d_border_point_functional = marker_ok &&
        state->texture3d_border_point_mismatches == 0;

    marker_ok = run_texture3d_output_probe(
        state, state->texture3d_border_compute_shader,
        state->texture3d_border_srv, state->border_linear_sampler,
        12, 0xffff00ffu, state->texture3d_border_linear_values,
        "BorderColor linear");
    for (i = 0; i < 12; ++i)
        if (!gray_value_matches(state->texture3d_border_linear_values[i],
                                linear_expected[i], 2))
            ++state->texture3d_border_linear_mismatches;
    state->texture3d_border_linear_functional = marker_ok &&
        state->texture3d_border_linear_mismatches == 0;

    fprintf(stderr,
            "winehua_d3d11_smoke: Texture3D boundary result "
            "load=%u index=%u pointBorder=%u linearBorder=%u "
            "mismatches=%u,%u,%u,%u\n",
            state->texture3d_oob_load_functional,
            state->texture3d_oob_index_functional,
            state->texture3d_border_point_functional,
            state->texture3d_border_linear_functional,
            state->texture3d_oob_load_mismatches,
            state->texture3d_oob_index_mismatches,
            state->texture3d_border_point_mismatches,
            state->texture3d_border_linear_mismatches);
    return state->texture3d_oob_load_functional &&
           state->texture3d_oob_index_functional &&
           state->texture3d_border_point_functional &&
           state->texture3d_border_linear_functional;
}

static BOOL run_heaven_fullscreen_probe_tolerance(
    struct smoke_state *state,
    ID3D11PixelShader *shader,
    ID3D11ShaderResourceView *resource,
    ID3D11SamplerState *sampler,
    const UINT *x, const UINT *y, const UINT *expected, UINT count,
    UINT *values, UINT *mismatch_count, BYTE tolerance, const char *name)
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
    UINT i, mismatches = 0;

    ID3D11DeviceContext_OMSetRenderTargets(state->context, 1, targets, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(
        state->context, state->probe_rtv, marker);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(state->context, shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(
        state->context, 0, 1, resources);
    if (sampler)
        ID3D11DeviceContext_PSSetSamplers(
            state->context, 0, 1, samplers);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(
        state->context, 0, 1, null_resources);
    if (sampler)
        ID3D11DeviceContext_PSSetSamplers(
            state->context, 0, 1, null_samplers);
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
        *mismatch_count = ~0u;
        fprintf(stderr,
                "winehua_d3d11_smoke: Heaven %s map failed=0x%08lx\n",
                name, (unsigned long)result);
        return FALSE;
    }
    for (i = 0; i < count; ++i)
    {
        values[i] = rgba8_value(&mapped, x[i], y[i]);
        if (!packed_value_matches(values[i], expected[i], tolerance))
            ++mismatches;
    }
    ID3D11DeviceContext_Unmap(
        state->context, (ID3D11Resource *)state->probe_staging, 0);
    state->feature_probe_read_bytes += WINEHUA_FEATURE_PROBE_SIZE *
                                       WINEHUA_FEATURE_PROBE_SIZE * 4;
    *mismatch_count = mismatches;
    fprintf(stderr, "winehua_d3d11_smoke: Heaven %s values=", name);
    for (i = 0; i < count; ++i)
        fprintf(stderr, "%s0x%08x", i ? "," : "", values[i]);
    fprintf(stderr, " mismatches=%u\n", mismatches);
    return mismatches == 0;
}

static BOOL run_heaven_fullscreen_probe(
    struct smoke_state *state,
    ID3D11PixelShader *shader,
    ID3D11ShaderResourceView *resource,
    ID3D11SamplerState *sampler,
    const UINT *x, const UINT *y, const UINT *expected, UINT count,
    UINT *values, UINT *mismatch_count, const char *name)
{
    return run_heaven_fullscreen_probe_tolerance(
        state, shader, resource, sampler, x, y, expected, count,
        values, mismatch_count, 0, name);
}

static void fill_heaven_cube_subresource(
    BYTE *data, DXGI_FORMAT format, UINT color, UINT texel_count)
{
    const BYTE red = (BYTE)color;
    const BYTE green = (BYTE)(color >> 8);
    const BYTE blue = (BYTE)(color >> 16);
    const BYTE alpha = (BYTE)(color >> 24);
    UINT i;

    memset(data, 0, 128);
    if (format == DXGI_FORMAT_BC1_UNORM ||
        format == DXGI_FORMAT_BC1_UNORM_SRGB)
    {
        const WORD rgb565 = (WORD)(((red * 31 / 255) << 11) |
                                   ((green * 63 / 255) << 5) |
                                   (blue * 31 / 255));
        data[0] = (BYTE)rgb565;
        data[1] = (BYTE)(rgb565 >> 8);
        return;
    }

    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        WORD *half = (WORD *)data;
        for (i = 0; i < texel_count; ++i)
        {
            half[i * 4 + 0] = red ? 0x3c00 : 0;
            half[i * 4 + 1] = green ? 0x3c00 : 0;
            half[i * 4 + 2] = blue ? 0x3c00 : 0;
            half[i * 4 + 3] = alpha ? 0x3c00 : 0;
        }
        return;
    }

    for (i = 0; i < texel_count; ++i)
        ((UINT *)data)[i] = color;
}

static BOOL create_heaven_cube_texture(
    struct smoke_state *state, DXGI_FORMAT format,
    ID3D11Texture2D **texture, ID3D11ShaderResourceView **srv)
{
    static const UINT colors[2][6] = {
        {0xff0000ffu, 0xff00ff00u, 0xffff0000u,
         0xff00ffffu, 0xffff00ffu, 0xffffff00u},
        {0xffffffffu, 0xff0000ffu, 0xff00ff00u,
         0xffff0000u, 0xff00ffffu, 0xffff00ffu},
    };
    BYTE storage[12][128];
    D3D11_SUBRESOURCE_DATA initial[12];
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    UINT face, mip;
    HRESULT result;

    memset(initial, 0, sizeof(initial));
    for (face = 0; face < 6; ++face)
    {
        for (mip = 0; mip < 2; ++mip)
        {
            const UINT subresource = mip + face * 2;
            const UINT width = 4 >> mip;
            const BOOL bc = format == DXGI_FORMAT_BC1_UNORM ||
                            format == DXGI_FORMAT_BC1_UNORM_SRGB;
            const UINT bytes_per_pixel =
                format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
            fill_heaven_cube_subresource(
                storage[subresource], format, colors[mip][face], width * width);
            initial[subresource].pSysMem = storage[subresource];
            initial[subresource].SysMemPitch = bc ? 8 : width * bytes_per_pixel;
            initial[subresource].SysMemSlicePitch = bc ? 8 :
                width * width * bytes_per_pixel;
        }
    }

    desc.Width = 4;
    desc.Height = 4;
    desc.MipLevels = 2;
    desc.ArraySize = 6;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    result = ID3D11Device_CreateTexture2D(
        state->device, &desc, initial, texture);
    if (FAILED(result))
    {
        state->present_result = result;
        return FALSE;
    }

    srv_desc.Format = format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srv_desc.TextureCube.MostDetailedMip = 0;
    srv_desc.TextureCube.MipLevels = 2;
    result = ID3D11Device_CreateShaderResourceView(
        state->device, (ID3D11Resource *)*texture, &srv_desc, srv);
    if (FAILED(result))
    {
        state->present_result = result;
        ID3D11Texture2D_Release(*texture);
        *texture = NULL;
        return FALSE;
    }
    return TRUE;
}

static BOOL run_heaven_cube_probes(struct smoke_state *state)
{
    static const char *shader_source =
        "TextureCube inputCube : register(t0);"
        "SamplerState inputSampler : register(s0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "int face = min((int)(input.pos.x * 6.0 / 64.0), 5);"
        "float3 direction = float3(1.0, 0.0, 0.0);"
        "if (face == 1) direction = float3(-1.0, 0.0, 0.0);"
        "else if (face == 2) direction = float3(0.0, 1.0, 0.0);"
        "else if (face == 3) direction = float3(0.0, -1.0, 0.0);"
        "else if (face == 4) direction = float3(0.0, 0.0, 1.0);"
        "else if (face == 5) direction = float3(0.0, 0.0, -1.0);"
        "float lod = input.pos.y >= 32.0 ? 1.0 : 0.0;"
        "return inputCube.SampleLevel(inputSampler, direction, lod); }";
    static const DXGI_FORMAT formats[WINEHUA_HEAVEN_CUBE_FORMATS] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_BC1_UNORM,
        DXGI_FORMAT_BC1_UNORM_SRGB,
    };
    static const char *names[WINEHUA_HEAVEN_CUBE_FORMATS] = {
        "cube RGBA8", "cube RGBA8 sRGB", "cube RGBA16F",
        "cube BC1", "cube BC1 sRGB",
    };
    static const UINT x[12] = {5, 16, 26, 37, 48, 58,
                               5, 16, 26, 37, 48, 58};
    static const UINT y[12] = {16, 16, 16, 16, 16, 16,
                               48, 48, 48, 48, 48, 48};
    static const UINT expected[12] = {
        0xff0000ffu, 0xff00ff00u, 0xffff0000u,
        0xff00ffffu, 0xffff00ffu, 0xffffff00u,
        0xffffffffu, 0xff0000ffu, 0xff00ff00u,
        0xffff0000u, 0xff00ffffu, 0xffff00ffu,
    };
    ID3DBlob *blob = NULL;
    ID3D11PixelShader *shader = NULL;
    HRESULT result;
    BOOL pass = TRUE;
    UINT i;

    if (!compile_shader(shader_source, "main", "ps_5_0", &blob)) return FALSE;
    result = ID3D11Device_CreatePixelShader(
        state->device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &shader);
    ID3D10Blob_Release(blob);
    if (FAILED(result)) return FALSE;

    for (i = 0; i < WINEHUA_HEAVEN_CUBE_FORMATS; ++i)
    {
        ID3D11Texture2D *texture = NULL;
        ID3D11ShaderResourceView *srv = NULL;
        UINT values[12] = {0};

        if (create_heaven_cube_texture(
                state, formats[i], &texture, &srv))
            state->heaven_cube_cases[i] = run_heaven_fullscreen_probe(
                state, shader, srv, state->bc_sampler,
                x, y, expected, 12, values,
                &state->heaven_cube_mismatches[i], names[i]);
        else
            state->heaven_cube_mismatches[i] = ~0u;

        if (!state->heaven_cube_cases[i]) pass = FALSE;
        if (srv) ID3D11ShaderResourceView_Release(srv);
        if (texture) ID3D11Texture2D_Release(texture);
    }
    ID3D11PixelShader_Release(shader);
    state->heaven_cube_functional = pass;
    return pass;
}

static BOOL run_heaven_texture3d_case(
    struct smoke_state *state, ID3D11PixelShader *shader,
    DXGI_FORMAT format, const BYTE values[4][2], UINT *actual,
    const UINT *expected, const char *name)
{
    static const UINT x[4] = {16, 48, 16, 48};
    static const UINT y[4] = {16, 16, 48, 48};
    static const UINT coords[4][3] = {
        {0, 0, 0}, {31, 0, 0}, {0, 31, 31}, {31, 31, 31},
    };
    const UINT size = 32;
    const UINT bytes_per_pixel = format == DXGI_FORMAT_R8G8_UNORM ? 2 : 1;
    const SIZE_T data_size = size * size * size * bytes_per_pixel;
    BYTE *data = calloc(1, data_size);
    D3D11_TEXTURE3D_DESC desc = {0};
    D3D11_SUBRESOURCE_DATA initial = {0};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    ID3D11Texture3D *texture = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    UINT mismatch_count = 0;
    BOOL pass = FALSE;
    HRESULT result;
    UINT i;

    if (!data) return FALSE;
    for (i = 0; i < 4; ++i)
    {
        const SIZE_T offset = ((coords[i][2] * size + coords[i][1]) * size +
                               coords[i][0]) * bytes_per_pixel;
        data[offset] = values[i][0];
        if (bytes_per_pixel == 2) data[offset + 1] = values[i][1];
    }

    desc.Width = size;
    desc.Height = size;
    desc.Depth = size;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    initial.pSysMem = data;
    initial.SysMemPitch = size * bytes_per_pixel;
    initial.SysMemSlicePitch = size * size * bytes_per_pixel;
    result = ID3D11Device_CreateTexture3D(
        state->device, &desc, &initial, &texture);
    free(data);
    if (FAILED(result)) return FALSE;

    srv_desc.Format = format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    srv_desc.Texture3D.MostDetailedMip = 0;
    srv_desc.Texture3D.MipLevels = 1;
    result = ID3D11Device_CreateShaderResourceView(
        state->device, (ID3D11Resource *)texture, &srv_desc, &srv);
    if (SUCCEEDED(result))
        pass = run_heaven_fullscreen_probe(
            state, shader, srv, NULL, x, y, expected, 4, actual,
            &mismatch_count, name);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    ID3D11Texture3D_Release(texture);
    return pass;
}

static BOOL run_heaven_texture3d_probes(struct smoke_state *state)
{
    static const char *shader_source =
        "Texture3D<float4> inputVolume : register(t0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "int slot = (input.pos.x >= 32.0) + ((input.pos.y >= 32.0) << 1);"
        "int3 coord = int3(0, 0, 0);"
        "if (slot == 1) coord = int3(31, 0, 0);"
        "else if (slot == 2) coord = int3(0, 31, 31);"
        "else if (slot == 3) coord = int3(31, 31, 31);"
        "float4 value = inputVolume.Load(int4(coord, 0));"
        "return float4(value.r, value.g, 0.0, 1.0); }";
    static const BYTE r8_values[4][2] = {
        {0x20, 0}, {0x60, 0}, {0xa0, 0}, {0xe0, 0},
    };
    static const BYTE rg8_values[4][2] = {
        {0x10, 0x20}, {0x40, 0x60},
        {0x80, 0xa0}, {0xc0, 0xe0},
    };
    static const UINT r8_expected[4] = {
        0xff000020u, 0xff000060u, 0xff0000a0u, 0xff0000e0u,
    };
    static const UINT rg8_expected[4] = {
        0xff002010u, 0xff006040u, 0xff00a080u, 0xff00e0c0u,
    };
    ID3DBlob *blob = NULL;
    ID3D11PixelShader *shader = NULL;
    HRESULT result;

    if (!compile_shader(shader_source, "main", "ps_5_0", &blob)) return FALSE;
    result = ID3D11Device_CreatePixelShader(
        state->device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &shader);
    ID3D10Blob_Release(blob);
    if (FAILED(result)) return FALSE;

    state->heaven_texture3d_r8_functional = run_heaven_texture3d_case(
        state, shader, DXGI_FORMAT_R8_UNORM, r8_values,
        state->heaven_texture3d_r8_values, r8_expected, "Texture3D R8");
    state->heaven_texture3d_rg8_functional = run_heaven_texture3d_case(
        state, shader, DXGI_FORMAT_R8G8_UNORM, rg8_values,
        state->heaven_texture3d_rg8_values, rg8_expected, "Texture3D RG8");
    ID3D11PixelShader_Release(shader);
    return state->heaven_texture3d_r8_functional &&
           state->heaven_texture3d_rg8_functional;
}

static BOOL run_heaven_comparison_sampler_probe(struct smoke_state *state)
{
    static const char *shader_source =
        "Texture2D<float> inputDepth : register(t0);"
        "SamplerComparisonState comparisonSampler : register(s0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "float2 uv = float2(input.pos.x >= 32.0 ? 0.75 : 0.25,"
        "                   input.pos.y >= 32.0 ? 0.75 : 0.25);"
        "float result = inputDepth.SampleCmpLevelZero("
        "    comparisonSampler, uv, 0.5);"
        "return float4(result, result, result, 1.0); }";
    static const float depth_values[4] = {0.2f, 0.4f, 0.6f, 0.8f};
    static const UINT x[4] = {16, 48, 16, 48};
    static const UINT y[4] = {16, 16, 48, 48};
    static const UINT expected[4] = {
        0xff000000u, 0xff000000u, 0xffffffffu, 0xffffffffu,
    };
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_SUBRESOURCE_DATA initial = {depth_values, sizeof(float) * 2, 0};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    D3D11_SAMPLER_DESC sampler_desc = {0};
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11SamplerState *sampler = NULL;
    ID3DBlob *blob = NULL;
    ID3D11PixelShader *shader = NULL;
    UINT mismatch_count = 0;
    HRESULT result;

    if (!compile_shader(shader_source, "main", "ps_5_0", &blob)) return FALSE;
    result = ID3D11Device_CreatePixelShader(
        state->device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &shader);
    ID3D10Blob_Release(blob);
    if (FAILED(result)) return FALSE;

    desc.Width = 2;
    desc.Height = 2;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    result = ID3D11Device_CreateTexture2D(
        state->device, &desc, &initial, &texture);
    if (SUCCEEDED(result))
    {
        srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)texture, &srv_desc, &srv);
    }
    if (SUCCEEDED(result))
    {
        sampler_desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        result = ID3D11Device_CreateSamplerState(
            state->device, &sampler_desc, &sampler);
    }
    if (SUCCEEDED(result))
        state->heaven_comparison_sampler_functional =
            run_heaven_fullscreen_probe(
                state, shader, srv, sampler, x, y, expected, 4,
                state->heaven_comparison_sampler_values,
                &mismatch_count, "comparison sampler");

    if (sampler) ID3D11SamplerState_Release(sampler);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (texture) ID3D11Texture2D_Release(texture);
    ID3D11PixelShader_Release(shader);
    return state->heaven_comparison_sampler_functional;
}

static BOOL run_heaven_depth_comparison_sampler_case(
    struct smoke_state *state, DXGI_FORMAT resource_format,
    DXGI_FORMAT dsv_format, DXGI_FORMAT srv_format,
    UINT *values, const char *name)
{
    static const char *shader_source =
        "Texture2D<float> inputDepth : register(t0);"
        "SamplerComparisonState comparisonSampler : register(s0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "float reference = input.pos.x >= 32.0 ? 0.75 : 0.25;"
        "float result = inputDepth.SampleCmpLevelZero("
        "    comparisonSampler, float2(0.5, 0.5), reference);"
        "return float4(result, result, result, 1.0); }";
    static const UINT x[4] = {16, 48, 16, 48};
    static const UINT y[4] = {16, 16, 48, 48};
    static const UINT expected[4] = {
        0xffffffffu, 0xff000000u, 0xffffffffu, 0xff000000u,
    };
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    D3D11_SAMPLER_DESC sampler_desc = {0};
    ID3D11Texture2D *texture = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11SamplerState *sampler = NULL;
    ID3DBlob *blob = NULL;
    ID3D11PixelShader *shader = NULL;
    UINT mismatch_count = 0;
    BOOL functional = FALSE;
    HRESULT result;

    if (!compile_shader(shader_source, "main", "ps_5_0", &blob)) return FALSE;
    result = ID3D11Device_CreatePixelShader(
        state->device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &shader);
    ID3D10Blob_Release(blob);
    if (FAILED(result)) return FALSE;

    desc.Width = 2;
    desc.Height = 2;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = resource_format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    result = ID3D11Device_CreateTexture2D(
        state->device, &desc, NULL, &texture);
    if (SUCCEEDED(result))
    {
        dsv_desc.Format = dsv_format;
        dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        result = ID3D11Device_CreateDepthStencilView(
            state->device, (ID3D11Resource *)texture, &dsv_desc, &dsv);
    }
    if (SUCCEEDED(result))
    {
        ID3D11DeviceContext_ClearDepthStencilView(
            state->context, dsv, D3D11_CLEAR_DEPTH, 0.5f, 0);
        srv_desc.Format = srv_format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)texture, &srv_desc, &srv);
    }
    if (SUCCEEDED(result))
    {
        sampler_desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        result = ID3D11Device_CreateSamplerState(
            state->device, &sampler_desc, &sampler);
    }
    if (SUCCEEDED(result))
        functional = run_heaven_fullscreen_probe(
            state, shader, srv, sampler, x, y, expected, 4,
            values, &mismatch_count, name);

    if (sampler) ID3D11SamplerState_Release(sampler);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (dsv) ID3D11DepthStencilView_Release(dsv);
    if (texture) ID3D11Texture2D_Release(texture);
    ID3D11PixelShader_Release(shader);
    return functional;
}

static BOOL create_d24s8_depth_texture(
    struct smoke_state *state, UINT array_size, UINT misc_flags,
    const float *depth_values, ID3D11Texture2D **texture, const char *name)
{
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
    ID3D11DepthStencilView *dsv = NULL;
    HRESULT result;
    UINT layer;

    desc.Width = 4;
    desc.Height = 4;
    desc.MipLevels = 1;
    desc.ArraySize = array_size;
    desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = misc_flags;
    result = ID3D11Device_CreateTexture2D(
        state->device, &desc, NULL, texture);
    if (FAILED(result))
    {
        fprintf(stderr,
                "winehua_d3d11_smoke: Heaven %s texture failed=0x%08lx\n",
                name, (unsigned long)result);
        return FALSE;
    }

    dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    dsv_desc.Texture2DArray.MipSlice = 0;
    dsv_desc.Texture2DArray.ArraySize = 1;
    for (layer = 0; layer < array_size; ++layer)
    {
        dsv_desc.Texture2DArray.FirstArraySlice = layer;
        result = ID3D11Device_CreateDepthStencilView(
            state->device, (ID3D11Resource *)*texture, &dsv_desc, &dsv);
        if (FAILED(result))
        {
            fprintf(stderr,
                    "winehua_d3d11_smoke: Heaven %s DSV layer=%u failed=0x%08lx\n",
                    name, layer, (unsigned long)result);
            ID3D11Texture2D_Release(*texture);
            *texture = NULL;
            return FALSE;
        }
        ID3D11DeviceContext_ClearDepthStencilView(
            state->context, dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
            depth_values[layer], (UINT8)(0x40 + layer));
        ID3D11DepthStencilView_Release(dsv);
        dsv = NULL;
    }
    return TRUE;
}

static BOOL render_d24s8_cube_corner_pattern(
    struct smoke_state *state, ID3D11Texture2D *texture)
{
    static const float corner_depths[WINEHUA_D24_CUBE_ARRAY_LAYERS] = {
        0.875f, 0.875f, 0.875f, 0.125f, 0.125f, 0.125f,
        0.125f, 0.125f, 0.125f, 0.875f, 0.875f, 0.875f,
    };
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
    D3D11_DEPTH_STENCIL_DESC depth_desc = {0};
    D3D11_VIEWPORT viewport = {3.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    ID3D11DepthStencilState *depth_state = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    HRESULT result;
    UINT layer;

    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    result = ID3D11Device_CreateDepthStencilState(
        state->device, &depth_desc, &depth_state);
    if (FAILED(result))
        return FALSE;

    dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    dsv_desc.Texture2DArray.MipSlice = 0;
    dsv_desc.Texture2DArray.ArraySize = 1;
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_OMSetDepthStencilState(
        state->context, depth_state, 0);

    for (layer = 0; layer < WINEHUA_D24_CUBE_ARRAY_LAYERS; ++layer)
    {
        dsv_desc.Texture2DArray.FirstArraySlice = layer;
        result = ID3D11Device_CreateDepthStencilView(
            state->device, (ID3D11Resource *)texture, &dsv_desc, &dsv);
        if (FAILED(result))
            break;
        viewport.MinDepth = corner_depths[layer];
        viewport.MaxDepth = corner_depths[layer];
        ID3D11DeviceContext_RSSetViewports(
            state->context, 1, &viewport);
        ID3D11DeviceContext_OMSetRenderTargets(
            state->context, 0, NULL, dsv);
        ID3D11DeviceContext_Draw(state->context, 3, 0);
        ID3D11DeviceContext_OMSetRenderTargets(
            state->context, 0, NULL, NULL);
        ID3D11DepthStencilView_Release(dsv);
        dsv = NULL;
    }

    ID3D11DeviceContext_VSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetDepthStencilState(
        state->context, NULL, 0);
    if (dsv)
        ID3D11DepthStencilView_Release(dsv);
    ID3D11DeviceContext_OMSetRenderTargets(
        state->context, 0, NULL, NULL);
    ID3D11DepthStencilState_Release(depth_state);
    if (FAILED(result))
    {
        fprintf(stderr,
                "winehua_d3d11_smoke: D24S8 CubeArray corner pattern failed=0x%08lx\n",
                (unsigned long)result);
        return FALSE;
    }
    return TRUE;
}

static BOOL run_heaven_d24s8_extended_matrix(struct smoke_state *state)
{
    static const char *array_shader_source =
        "Texture2DArray<float> inputDepth : register(t0);"
        "SamplerComparisonState comparisonSampler : register(s0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "uint layer = min((uint)(input.pos.x * (6.0 / 64.0)), 5u);"
        "float value = inputDepth.SampleCmpLevelZero("
        " comparisonSampler, float3(0.5, 0.5, (float)layer), 0.5);"
        "return float4(value, value, value, 1.0); }";
    static const char *array_view_shader_source =
        "Texture2DArray<float> inputDepth : register(t0);"
        "SamplerComparisonState comparisonSampler : register(s0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "float value = inputDepth.SampleCmpLevelZero("
        " comparisonSampler, float3(0.5, 0.5, 0.0), 0.5);"
        "return float4(value, value, value, 1.0); }";
    static const char *cube_shader_source =
        "TextureCube<float> inputDepth : register(t0);"
        "SamplerComparisonState comparisonSampler : register(s0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float3 cubeDirection(uint face) {"
        " if (face == 0) return float3(1,0,0);"
        " if (face == 1) return float3(-1,0,0);"
        " if (face == 2) return float3(0,1,0);"
        " if (face == 3) return float3(0,-1,0);"
        " if (face == 4) return float3(0,0,1);"
        " return float3(0,0,-1); }"
        "float4 main(PSIn input) : SV_Target {"
        "uint face = min((uint)(input.pos.x * (6.0 / 64.0)), 5u);"
        "float value = inputDepth.SampleCmpLevelZero("
        " comparisonSampler, cubeDirection(face), 0.5);"
        "return float4(value, value, value, 1.0); }";
    static const char *cube_sample_shader_source =
        "TextureCube<float> inputDepth : register(t0);"
        "SamplerState regularSampler : register(s0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float3 cubeDirection(uint face) {"
        " if (face == 0) return float3(1,0,0);"
        " if (face == 1) return float3(-1,0,0);"
        " if (face == 2) return float3(0,1,0);"
        " if (face == 3) return float3(0,-1,0);"
        " if (face == 4) return float3(0,0,1);"
        " return float3(0,0,-1); }"
        "float4 main(PSIn input) : SV_Target {"
        "uint face = min((uint)(input.pos.x * (6.0 / 64.0)), 5u);"
        "float value = inputDepth.SampleLevel("
        " regularSampler, cubeDirection(face), 0.0);"
        "return float4(value, value, value, 1.0); }";
    static const char *cube_array_shader_source =
        "TextureCubeArray<float> inputDepth : register(t0);"
        "SamplerComparisonState comparisonSampler : register(s0);"
        "SamplerState regularSampler : register(s1);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float3 cubeDirection(uint face, float edge) {"
        " if (face == 0) return float3(1,edge,-edge);"
        " if (face == 1) return float3(-1,edge,edge);"
        " if (face == 2) return float3(edge,1,-edge);"
        " if (face == 3) return float3(edge,-1,edge);"
        " if (face == 4) return float3(edge,edge,1);"
        " return float3(-edge,edge,-1); }"
        "float4 main(PSIn input) : SV_Target {"
        "uint face = min((uint)(input.pos.x * (6.0 / 64.0)), 5u);"
        "float cube = input.pos.y >= 32.0 ? 1.0 : 0.0;"
        "float4 coord = float4(cubeDirection(face, 0.75), cube);"
        "float sampled = inputDepth.SampleLevel(regularSampler, coord, 0.0);"
        "float explicitCmp = inputDepth.SampleCmpLevelZero("
        " comparisonSampler, coord, 0.5);"
        "float implicitCmp = inputDepth.SampleCmp("
        " comparisonSampler, coord, 0.5);"
        "float4 gathered = inputDepth.GatherCmp(comparisonSampler, float4(cubeDirection(face, 0.0), cube), 0.5);"
        "float gatherExpected = 1.0 - explicitCmp;"
        "float4 expectedGather = float4(gatherExpected, gatherExpected, gatherExpected, gatherExpected);"
        "float gatherError = dot(abs(gathered - expectedGather), float4(0.25, 0.25, 0.25, 0.25));"
        "return float4(saturate(sampled + gatherError), explicitCmp, implicitCmp, 1.0); }";
    static const char *linear_border_shader_source =
        "Texture2DArray<float> inputDepth : register(t0);"
        "SamplerComparisonState comparisonSampler : register(s0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        "float2 coord = float2(0.5, 0.5);"
        "if (input.pos.x >= 16.0 && input.pos.x < 32.0) coord = float2(-1.0, 0.5);"
        "else if (input.pos.x >= 32.0 && input.pos.x < 48.0) coord = float2(0.0, 0.5);"
        "else if (input.pos.x >= 48.0) coord = float2(1.0, 0.5);"
        "float value = inputDepth.SampleCmpLevelZero("
        " comparisonSampler, float3(coord, 0.0), 0.5);"
        "return float4(value, value, value, 1.0); }";
    static const float array_depths[WINEHUA_D24_ARRAY_LAYERS] = {
        0.125f, 0.25f, 0.375f, 0.625f, 0.75f, 0.875f,
    };
    static const float cube_array_depths[WINEHUA_D24_CUBE_ARRAY_LAYERS] = {
        0.125f, 0.25f, 0.375f, 0.625f, 0.75f, 0.875f,
        0.875f, 0.75f, 0.625f, 0.375f, 0.25f, 0.125f,
    };
    static const UINT stripe_x[WINEHUA_D24_ARRAY_LAYERS] = {
        5, 16, 27, 37, 48, 58,
    };
    static const UINT stripe_y[WINEHUA_D24_ARRAY_LAYERS] = {
        16, 16, 16, 16, 16, 16,
    };
    static const UINT cube_array_x[WINEHUA_D24_CUBE_ARRAY_LAYERS] = {
        5, 16, 27, 37, 48, 58, 5, 16, 27, 37, 48, 58,
    };
    static const UINT cube_array_y[WINEHUA_D24_CUBE_ARRAY_LAYERS] = {
        16, 16, 16, 16, 16, 16, 48, 48, 48, 48, 48, 48,
    };
    static const UINT array_expected[WINEHUA_D24_ARRAY_LAYERS] = {
        0xff000000u, 0xff000000u, 0xff000000u,
        0xffffffffu, 0xffffffffu, 0xffffffffu,
    };
    static const UINT cube_sample_expected[WINEHUA_D24_ARRAY_LAYERS] = {
        0xff202020u, 0xff404040u, 0xff606060u,
        0xff9f9f9fu, 0xffbfbfbfu, 0xffdfdfdfu,
    };
    static const UINT cube_array_expected[WINEHUA_D24_CUBE_ARRAY_LAYERS] = {
        0xffffffdfu, 0xffffffdfu, 0xffffffdfu,
        0xff000020u, 0xff000020u, 0xff000020u,
        0xff000020u, 0xff000020u, 0xff000020u,
        0xffffffdfu, 0xffffffdfu, 0xffffffdfu,
    };
    static const UINT border_x[WINEHUA_D24_BORDER_CASES] = {8, 24, 40, 56};
    static const UINT border_y[WINEHUA_D24_BORDER_CASES] = {32, 32, 32, 32};
    static const UINT border_expected[WINEHUA_D24_BORDER_CASES] = {
        0xffffffffu, 0xff000000u, 0xff808080u, 0xff808080u,
    };
    static const UINT single_x = 32;
    static const UINT single_y = 32;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    D3D11_SAMPLER_DESC sampler_desc = {0};
    ID3D11Texture2D *array_texture = NULL;
    ID3D11Texture2D *cube_texture = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11SamplerState *linear_clamp_sampler = NULL;
    ID3D11SamplerState *linear_border_sampler = NULL;
    ID3D11SamplerState *regular_linear_sampler = NULL;
    ID3D11PixelShader *array_shader = NULL;
    ID3D11PixelShader *array_view_shader = NULL;
    ID3D11PixelShader *cube_shader = NULL;
    ID3D11PixelShader *cube_sample_shader = NULL;
    ID3D11PixelShader *cube_array_shader = NULL;
    ID3D11PixelShader *linear_border_shader = NULL;
    HRESULT result = E_FAIL;
    UINT layer;
    UINT mismatches;

    array_shader = create_probe_pixel_shader(
        state, array_shader_source, "D24S8 array");
    array_view_shader = create_probe_pixel_shader(
        state, array_view_shader_source, "D24S8 array view");
    cube_shader = create_probe_pixel_shader(
        state, cube_shader_source, "D24S8 cube");
    cube_sample_shader = create_probe_pixel_shader(
        state, cube_sample_shader_source, "D24S8 cube regular sample");
    cube_array_shader = create_probe_pixel_shader(
        state, cube_array_shader_source, "D24S8 cube array");
    linear_border_shader = create_probe_pixel_shader(
        state, linear_border_shader_source, "D24S8 linear border");
    if (!array_shader || !array_view_shader || !cube_shader ||
        !cube_sample_shader ||
        !cube_array_shader || !linear_border_shader)
        goto done;

    if (!create_d24s8_depth_texture(
            state, WINEHUA_D24_ARRAY_LAYERS, 0, array_depths,
            &array_texture, "D24S8 array") ||
        !create_d24s8_depth_texture(
            state, WINEHUA_D24_CUBE_ARRAY_LAYERS,
            D3D11_RESOURCE_MISC_TEXTURECUBE, cube_array_depths,
            &cube_texture, "D24S8 cube array"))
        goto done;

    /* Replace the cleared CubeArray values at one off-axis texel. This
     * distinguishes face/UV orientation from a test that samples only the
     * cube center, while leaving the independent 2D-array texture intact. */
    if (!render_d24s8_cube_corner_pattern(state, cube_texture))
        goto done;

    sampler_desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    result = ID3D11Device_CreateSamplerState(
        state->device, &sampler_desc, &linear_clamp_sampler);
    if (FAILED(result)) goto done;
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    result = ID3D11Device_CreateSamplerState(
        state->device, &sampler_desc, &regular_linear_sampler);
    if (FAILED(result)) goto done;
    sampler_desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    result = ID3D11Device_CreateSamplerState(
        state->device, &sampler_desc, &linear_border_sampler);
    if (FAILED(result)) goto done;

    srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srv_desc.Texture2DArray.MostDetailedMip = 0;
    srv_desc.Texture2DArray.MipLevels = 1;
    srv_desc.Texture2DArray.FirstArraySlice = 0;
    srv_desc.Texture2DArray.ArraySize = WINEHUA_D24_ARRAY_LAYERS;
    result = ID3D11Device_CreateShaderResourceView(
        state->device, (ID3D11Resource *)array_texture, &srv_desc, &srv);
    if (SUCCEEDED(result))
        state->heaven_d24s8_array_functional =
            run_heaven_fullscreen_probe(
                state, array_shader, srv, linear_clamp_sampler,
                stripe_x, stripe_y, array_expected,
                WINEHUA_D24_ARRAY_LAYERS,
                state->heaven_d24s8_array_values,
                &state->heaven_d24s8_extended_mismatches[0],
                "D24S8 Texture2DArray linear comparison");
    if (srv) ID3D11ShaderResourceView_Release(srv);
    srv = NULL;

    state->heaven_d24s8_array_views_functional = TRUE;
    for (layer = 0; layer < WINEHUA_D24_ARRAY_LAYERS; ++layer)
    {
        srv_desc.Texture2DArray.FirstArraySlice = layer;
        srv_desc.Texture2DArray.ArraySize = 1;
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)array_texture, &srv_desc, &srv);
        mismatches = 0;
        if (FAILED(result) ||
            !run_heaven_fullscreen_probe(
                state, array_view_shader, srv, linear_clamp_sampler,
                &single_x, &single_y, &array_expected[layer], 1,
                &state->heaven_d24s8_array_view_values[layer],
                &mismatches, "D24S8 single-layer view"))
            state->heaven_d24s8_array_views_functional = FALSE;
        state->heaven_d24s8_extended_mismatches[1] += mismatches;
        if (srv) ID3D11ShaderResourceView_Release(srv);
        srv = NULL;
    }

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srv_desc.Texture2DArray.MostDetailedMip = 0;
    srv_desc.Texture2DArray.MipLevels = 1;
    srv_desc.Texture2DArray.FirstArraySlice = 0;
    srv_desc.Texture2DArray.ArraySize = WINEHUA_D24_ARRAY_LAYERS;
    result = ID3D11Device_CreateShaderResourceView(
        state->device, (ID3D11Resource *)cube_texture, &srv_desc, &srv);
    if (SUCCEEDED(result))
        state->heaven_d24s8_cube_as_array_functional =
            run_heaven_fullscreen_probe(
                state, array_shader, srv, linear_clamp_sampler,
                stripe_x, stripe_y, array_expected,
                WINEHUA_D24_ARRAY_LAYERS,
                state->heaven_d24s8_cube_as_array_values,
                &state->heaven_d24s8_extended_mismatches[2],
                "D24S8 cube-compatible image as Texture2DArray");
    if (srv) ID3D11ShaderResourceView_Release(srv);
    srv = NULL;

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srv_desc.TextureCube.MostDetailedMip = 0;
    srv_desc.TextureCube.MipLevels = 1;
    result = ID3D11Device_CreateShaderResourceView(
        state->device, (ID3D11Resource *)cube_texture, &srv_desc, &srv);
    if (SUCCEEDED(result))
    {
        state->heaven_d24s8_cube_sample_functional =
            run_heaven_fullscreen_probe_tolerance(
                state, cube_sample_shader, srv, regular_linear_sampler,
                stripe_x, stripe_y, cube_sample_expected,
                WINEHUA_D24_ARRAY_LAYERS,
                state->heaven_d24s8_cube_sample_values,
                &state->heaven_d24s8_extended_mismatches[3], 2,
                "D24S8 TextureCube regular linear sample");
        state->heaven_d24s8_cube_functional =
            run_heaven_fullscreen_probe(
                state, cube_shader, srv, linear_clamp_sampler,
                stripe_x, stripe_y, array_expected,
                WINEHUA_D24_ARRAY_LAYERS,
                state->heaven_d24s8_cube_values,
                &state->heaven_d24s8_extended_mismatches[4],
                "D24S8 TextureCube linear comparison");
    }
    if (srv) ID3D11ShaderResourceView_Release(srv);
    srv = NULL;

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srv_desc.Texture2DArray.MostDetailedMip = 0;
    srv_desc.Texture2DArray.MipLevels = 1;
    srv_desc.Texture2DArray.FirstArraySlice = 4;
    srv_desc.Texture2DArray.ArraySize = 1;
    result = ID3D11Device_CreateShaderResourceView(
        state->device, (ID3D11Resource *)array_texture, &srv_desc, &srv);
    if (SUCCEEDED(result))
        state->heaven_d24s8_linear_border_functional =
            run_heaven_fullscreen_probe_tolerance(
                state, linear_border_shader, srv, linear_border_sampler,
                border_x, border_y, border_expected,
                WINEHUA_D24_BORDER_CASES,
                state->heaven_d24s8_linear_border_values,
                &state->heaven_d24s8_extended_mismatches[6], 2,
                "D24S8 linear comparison border");
    if (srv) ID3D11ShaderResourceView_Release(srv);
    srv = NULL;

    write_state(state, "RUNNING", "dxvk", "D24S8 cube-array pending");
    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    srv_desc.TextureCubeArray.MostDetailedMip = 0;
    srv_desc.TextureCubeArray.MipLevels = 1;
    srv_desc.TextureCubeArray.First2DArrayFace = 0;
    srv_desc.TextureCubeArray.NumCubes = 2;
    result = ID3D11Device_CreateShaderResourceView(
        state->device, (ID3D11Resource *)cube_texture, &srv_desc, &srv);
    if (SUCCEEDED(result))
    {
        ID3D11SamplerState *regular_samplers[] = {regular_linear_sampler};
        ID3D11SamplerState *null_samplers[] = {NULL};
        ID3D11DeviceContext_PSSetSamplers(
            state->context, 1, 1, regular_samplers);
        state->heaven_d24s8_cube_array_functional =
            run_heaven_fullscreen_probe_tolerance(
                state, cube_array_shader, srv, linear_clamp_sampler,
                cube_array_x, cube_array_y, cube_array_expected,
                WINEHUA_D24_CUBE_ARRAY_LAYERS,
                state->heaven_d24s8_cube_array_values,
                &state->heaven_d24s8_extended_mismatches[5], 2,
                "D24S8 TextureCubeArray mixed sample and comparison");
        ID3D11DeviceContext_PSSetSamplers(
            state->context, 1, 1, null_samplers);
    }

done:
    if (FAILED(result))
        fprintf(stderr,
                "winehua_d3d11_smoke: Heaven D24S8 extended resource creation failed=0x%08lx\n",
                (unsigned long)result);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (linear_border_sampler)
        ID3D11SamplerState_Release(linear_border_sampler);
    if (linear_clamp_sampler)
        ID3D11SamplerState_Release(linear_clamp_sampler);
    if (regular_linear_sampler)
        ID3D11SamplerState_Release(regular_linear_sampler);
    if (cube_texture) ID3D11Texture2D_Release(cube_texture);
    if (array_texture) ID3D11Texture2D_Release(array_texture);
    if (linear_border_shader)
        ID3D11PixelShader_Release(linear_border_shader);
    if (cube_array_shader) ID3D11PixelShader_Release(cube_array_shader);
    if (cube_sample_shader) ID3D11PixelShader_Release(cube_sample_shader);
    if (cube_shader) ID3D11PixelShader_Release(cube_shader);
    if (array_view_shader) ID3D11PixelShader_Release(array_view_shader);
    if (array_shader) ID3D11PixelShader_Release(array_shader);

    state->heaven_d24s8_extended_functional =
        state->heaven_d24s8_array_functional &&
        state->heaven_d24s8_array_views_functional &&
        state->heaven_d24s8_cube_as_array_functional &&
        state->heaven_d24s8_cube_sample_functional &&
        state->heaven_d24s8_cube_functional &&
        state->heaven_d24s8_cube_array_functional &&
        state->heaven_d24s8_linear_border_functional;
    fprintf(stderr,
            "winehua_d3d11_smoke: Heaven D24S8 extended result "
            "array=%u views=%u cubeAsArray=%u cubeSample=%u "
            "cubeCompare=%u cubeArray=%u linearBorder=%u\n",
            state->heaven_d24s8_array_functional,
            state->heaven_d24s8_array_views_functional,
            state->heaven_d24s8_cube_as_array_functional,
            state->heaven_d24s8_cube_sample_functional,
            state->heaven_d24s8_cube_functional,
            state->heaven_d24s8_cube_array_functional,
            state->heaven_d24s8_linear_border_functional);
    return state->heaven_d24s8_extended_functional;
}

static BOOL run_heaven_resource_probes(struct smoke_state *state)
{
    const BOOL cube = run_heaven_cube_probes(state);
    const BOOL texture3d = run_heaven_texture3d_probes(state);
    const BOOL comparison = run_heaven_comparison_sampler_probe(state);
    BOOL depth_comparison;
    BOOL d24s8_depth_comparison;
    BOOL d24s8_extended;

    depth_comparison = run_heaven_depth_comparison_sampler_case(
        state, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT,
        DXGI_FORMAT_R32_FLOAT,
        state->heaven_depth_comparison_sampler_values,
        "D32 depth comparison sampler");
    state->heaven_depth_comparison_sampler_functional = depth_comparison;

    d24s8_depth_comparison = run_heaven_depth_comparison_sampler_case(
        state, DXGI_FORMAT_R24G8_TYPELESS,
        DXGI_FORMAT_D24_UNORM_S8_UINT,
        DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
        state->heaven_d24s8_depth_comparison_sampler_values,
        "D24S8 depth comparison sampler");
    state->heaven_d24s8_depth_comparison_sampler_functional =
        d24s8_depth_comparison;

    /* The extended matrix emits a RUNNING checkpoint before its CubeArray
     * draw. Publish the completed 2D depth-comparison cases first so a crash
     * in that later probe cannot leave already verified cases recorded as
     * pending/false in the authoritative smoke JSON. */
    d24s8_extended = run_heaven_d24s8_extended_matrix(state);
    state->heaven_resource_functional =
        cube && texture3d && depth_comparison && d24s8_depth_comparison &&
        d24s8_extended;
    if (!comparison)
        fprintf(stderr,
                "winehua_d3d11_smoke: ordinary R32_FLOAT comparison sampler "
                "is diagnostic-only; Heaven depth comparison gates remain authoritative\n");
    return state->heaven_resource_functional;
}

static void run_heaven_probe_pass(
    struct smoke_state *state, ID3D11PixelShader *pixel_shader,
    ID3D11ShaderResourceView *const *resources, UINT resource_count,
    ID3D11SamplerState *const *samplers, UINT sampler_count,
    ID3D11DepthStencilView *dsv, ID3D11DepthStencilState *depth_state)
{
    D3D11_VIEWPORT viewport = {0, 0, WINEHUA_FEATURE_PROBE_SIZE,
                               WINEHUA_FEATURE_PROBE_SIZE, 0.0f, 1.0f};
    ID3D11ShaderResourceView *null_resources[8] = {0};
    ID3D11SamplerState *null_samplers[8] = {0};
    ID3D11RenderTargetView *targets[] = {state->probe_rtv};

    ID3D11DeviceContext_ClearRenderTargetView(
        state->context, state->probe_rtv, (const float[]){0, 0, 0, 1});
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 1, targets, dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(
        state->context, depth_state, 0);
    ID3D11DeviceContext_OMSetBlendState(
        state->context, NULL, NULL, 0xffffffffu);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(
        state->context, pixel_shader, NULL, 0);
    if (resource_count)
        ID3D11DeviceContext_PSSetShaderResources(
            state->context, 0, resource_count, resources);
    if (sampler_count)
        ID3D11DeviceContext_PSSetSamplers(
            state->context, 0, sampler_count, samplers);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    if (resource_count)
        ID3D11DeviceContext_PSSetShaderResources(
            state->context, 0, resource_count, null_resources);
    if (sampler_count)
        ID3D11DeviceContext_PSSetSamplers(
            state->context, 0, sampler_count, null_samplers);
    ID3D11DeviceContext_PSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);
    ID3D11DeviceContext_OMSetDepthStencilState(state->context, NULL, 0);
}

static BOOL read_heaven_probe_points(
    struct smoke_state *state, const UINT *x, const UINT *y,
    UINT count, UINT *values, const char *name)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT result;
    UINT i;

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
        fprintf(stderr,
                "winehua_d3d11_smoke: %s map failed=0x%08lx\n",
                name, (unsigned long)result);
        state->present_result = result;
        return FALSE;
    }
    fprintf(stderr, "winehua_d3d11_smoke: %s values=", name);
    for (i = 0; i < count; ++i)
    {
        values[i] = rgba8_value(&mapped, x[i], y[i]);
        fprintf(stderr, "%s0x%08x", i ? "," : "", values[i]);
    }
    fprintf(stderr, "\n");
    ID3D11DeviceContext_Unmap(
        state->context, (ID3D11Resource *)state->probe_staging, 0);
    state->feature_probe_read_bytes += WINEHUA_FEATURE_PROBE_SIZE *
                                       WINEHUA_FEATURE_PROBE_SIZE * 4;
    return TRUE;
}

static BOOL run_mrt_gbuffer_smoke(struct smoke_state *state)
{
    static const char *mrt_shader_source =
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "struct PSOut { float4 rt0 : SV_Target0; float4 rt1 : SV_Target1;"
        " float4 rt2 : SV_Target2; float depth : SV_Depth; };"
        "PSOut main(PSIn input) { PSOut output;"
        " output.rt0 = float4(1,0,0,1);"
        " output.rt1 = float4(0,1,0,1);"
        " output.rt2 = float4(0,0,1,1);"
        " output.depth = saturate(input.pos.x / 64.0); return output; }";
    static const char *sample_shader_source =
        "Texture2D<float4> rt0 : register(t0);"
        "Texture2D<float4> rt1 : register(t1);"
        "Texture2D<float4> rt2 : register(t2);"
        "Texture2D<float> depthTexture : register(t3);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target { int3 p = int3(input.pos.xy,0);"
        " bool right = input.pos.x >= 32.0; bool bottom = input.pos.y >= 32.0;"
        " if (!bottom && !right) return rt0.Load(p);"
        " if (!bottom && right) return rt1.Load(p);"
        " if (bottom && !right) return rt2.Load(p);"
        " float depth = depthTexture.Load(p);"
        " return float4(depth,depth,depth,1); }";
    static const UINT x[WINEHUA_MRT_PROBE_POINTS] = {16, 48, 16, 48};
    static const UINT y[WINEHUA_MRT_PROBE_POINTS] = {16, 16, 48, 48};
    static const UINT expected[3] = {
        0xff0000ffu, 0xff00ff00u, 0xffff0000u,
    };
    D3D11_TEXTURE2D_DESC color_desc = {0};
    D3D11_TEXTURE2D_DESC depth_desc = {0};
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
    D3D11_SHADER_RESOURCE_VIEW_DESC depth_srv_desc = {0};
    D3D11_VIEWPORT viewport = {0, 0, WINEHUA_FEATURE_PROBE_SIZE,
                               WINEHUA_FEATURE_PROBE_SIZE, 0.0f, 1.0f};
    ID3D11Texture2D *colors[3] = {0};
    ID3D11RenderTargetView *rtvs[3] = {0};
    ID3D11ShaderResourceView *srvs[4] = {0};
    ID3D11Texture2D *depth = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11PixelShader *mrt_shader = NULL;
    ID3D11PixelShader *sample_shader = NULL;
    UINT i;
    HRESULT result = E_FAIL;

    state->mrt_gbuffer_mismatches = 0;
    mrt_shader = create_probe_pixel_shader(
        state, mrt_shader_source, "MRT G-buffer output");
    sample_shader = create_probe_pixel_shader(
        state, sample_shader_source, "MRT G-buffer sample");
    if (!mrt_shader || !sample_shader) goto done;

    color_desc.Width = WINEHUA_FEATURE_PROBE_SIZE;
    color_desc.Height = WINEHUA_FEATURE_PROBE_SIZE;
    color_desc.MipLevels = 1;
    color_desc.ArraySize = 1;
    color_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_desc.SampleDesc.Count = 1;
    color_desc.Usage = D3D11_USAGE_DEFAULT;
    color_desc.BindFlags = D3D11_BIND_RENDER_TARGET |
                           D3D11_BIND_SHADER_RESOURCE;
    result = S_OK;
    for (i = 0; i < 3 && SUCCEEDED(result); ++i)
    {
        result = ID3D11Device_CreateTexture2D(
            state->device, &color_desc, NULL, &colors[i]);
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateRenderTargetView(
                state->device, (ID3D11Resource *)colors[i], NULL, &rtvs[i]);
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateShaderResourceView(
                state->device, (ID3D11Resource *)colors[i], NULL, &srvs[i]);
    }

    depth_desc = color_desc;
    depth_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL |
                           D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateTexture2D(
            state->device, &depth_desc, NULL, &depth);
    dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateDepthStencilView(
            state->device, (ID3D11Resource *)depth, &dsv_desc, &dsv);
    depth_srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depth_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depth_srv_desc.Texture2D.MipLevels = 1;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)depth, &depth_srv_desc, &srvs[3]);
    if (FAILED(result)) goto done;

    ID3D11DeviceContext_OMSetRenderTargets(state->context, 3, rtvs, dsv);
    for (i = 0; i < 3; ++i)
        ID3D11DeviceContext_ClearRenderTargetView(
            state->context, rtvs[i], (const float[]){0, 0, 0, 1});
    ID3D11DeviceContext_ClearDepthStencilView(
        state->context, dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ID3D11DeviceContext_OMSetDepthStencilState(state->context, NULL, 0);
    ID3D11DeviceContext_OMSetBlendState(
        state->context, NULL, NULL, 0xffffffffu);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(state->context, mrt_shader, NULL, 0);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_PSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);

    run_heaven_probe_pass(
        state, sample_shader, srvs, 4, NULL, 0, NULL, NULL);
    if (!read_heaven_probe_points(
            state, x, y, WINEHUA_MRT_PROBE_POINTS,
            state->mrt_gbuffer_values, "MRT G-buffer"))
        goto done;
    for (i = 0; i < 3; ++i)
        if (state->mrt_gbuffer_values[i] != expected[i])
            state->mrt_gbuffer_mismatches++;
    {
        UINT value = state->mrt_gbuffer_values[3];
        UINT red = value & 0xffu;
        UINT green = (value >> 8) & 0xffu;
        UINT blue = (value >> 16) & 0xffu;
        UINT alpha = value >> 24;
        if (red < 185 || red > 200 || green != red || blue != red ||
            alpha != 0xffu)
            state->mrt_gbuffer_mismatches++;
    }
    state->mrt_gbuffer_functional =
        state->mrt_gbuffer_mismatches == 0;

done:
    if (FAILED(result))
    {
        state->present_result = result;
        fprintf(stderr,
                "winehua_d3d11_smoke: MRT G-buffer resource failure=0x%08lx\n",
                (unsigned long)result);
    }
    fprintf(stderr,
            "winehua_d3d11_smoke: MRT G-buffer mismatches=%u pass=%u\n",
            state->mrt_gbuffer_mismatches, state->mrt_gbuffer_functional);
    if (sample_shader) ID3D11PixelShader_Release(sample_shader);
    if (mrt_shader) ID3D11PixelShader_Release(mrt_shader);
    if (dsv) ID3D11DepthStencilView_Release(dsv);
    if (srvs[3]) ID3D11ShaderResourceView_Release(srvs[3]);
    if (depth) ID3D11Texture2D_Release(depth);
    for (i = 0; i < 3; ++i)
    {
        if (srvs[i]) ID3D11ShaderResourceView_Release(srvs[i]);
        if (rtvs[i]) ID3D11RenderTargetView_Release(rtvs[i]);
        if (colors[i]) ID3D11Texture2D_Release(colors[i]);
    }
    return state->mrt_gbuffer_functional;
}

static BOOL run_d24_readonly_shadow_smoke(struct smoke_state *state)
{
    static const char *depth_write_source =
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float main(PSIn input) : SV_Depth {"
        " if (input.pos.x < 21.0) return 0.2;"
        " if (input.pos.x < 43.0) return 0.5; return 0.8; }";
    static const char *depth_rewrite_source =
        "float main(float4 pos : SV_Position) : SV_Depth { return 0.35; }";
    static const char *depth_sample_source =
        "Texture2D<float> inputDepth : register(t0);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target {"
        " float value = inputDepth.Load(int3(input.pos.xy,0));"
        " return float4(value,value,value,1); }";
    static const char *depth_compare_source =
        "Texture2D<float> inputDepth : register(t0);"
        "SamplerComparisonState lessSampler : register(s0);"
        "SamplerComparisonState lessEqualSampler : register(s1);"
        "SamplerComparisonState greaterSampler : register(s2);"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn input) : SV_Target { float value;"
        " if (input.pos.y < 21.0) value = inputDepth.SampleCmpLevelZero("
        "  lessSampler,input.uv,0.5);"
        " else if (input.pos.y < 43.0) value = inputDepth.SampleCmpLevelZero("
        "  lessEqualSampler,input.uv,0.5);"
        " else value = inputDepth.SampleCmpLevelZero("
        "  greaterSampler,input.uv,0.5);"
        " return float4(value,value,value,1); }";
    static const UINT depth_x[WINEHUA_D24_DEPTH_POINTS] = {10, 32, 54};
    static const UINT depth_y[WINEHUA_D24_DEPTH_POINTS] = {32, 32, 32};
    static const UINT compare_x[WINEHUA_D24_COMPARE_POINTS] = {
        10, 32, 54, 10, 32, 54, 10, 32, 54,
    };
    static const UINT compare_y[WINEHUA_D24_COMPARE_POINTS] = {
        10, 10, 10, 32, 32, 32, 54, 54, 54,
    };
    static const UINT compare_expected[WINEHUA_D24_COMPARE_POINTS] = {
        0xff000000u, 0xff000000u, 0xffffffffu,
        0xff000000u, 0xffffffffu, 0xffffffffu,
        0xffffffffu, 0xff000000u, 0xff000000u,
    };
    static const UINT initial_red[WINEHUA_D24_DEPTH_POINTS] = {51, 128, 204};
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    D3D11_DEPTH_STENCIL_DESC depth_state_desc = {0};
    D3D11_SAMPLER_DESC sampler_desc = {0};
    D3D11_VIEWPORT viewport = {0, 0, WINEHUA_FEATURE_PROBE_SIZE,
                               WINEHUA_FEATURE_PROBE_SIZE, 0.0f, 1.0f};
    ID3D11Texture2D *texture = NULL;
    ID3D11DepthStencilView *write_dsv = NULL;
    ID3D11DepthStencilView *read_dsv = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11DepthStencilState *read_depth_state = NULL;
    ID3D11SamplerState *samplers[3] = {0};
    ID3D11PixelShader *write_shader = NULL;
    ID3D11PixelShader *rewrite_shader = NULL;
    ID3D11PixelShader *sample_shader = NULL;
    ID3D11PixelShader *compare_shader = NULL;
    ID3D11ShaderResourceView *resources[] = {NULL};
    UINT i;
    HRESULT result = E_FAIL;

    state->d24_readonly_shadow_mismatches = 0;
    write_shader = create_probe_pixel_shader(
        state, depth_write_source, "D24 writable depth");
    rewrite_shader = create_probe_pixel_shader(
        state, depth_rewrite_source, "D24 writable depth rewrite");
    sample_shader = create_probe_pixel_shader(
        state, depth_sample_source, "D24 depth SRV");
    compare_shader = create_probe_pixel_shader(
        state, depth_compare_source, "D24 read-only SampleCmp");
    if (!write_shader || !rewrite_shader || !sample_shader || !compare_shader)
        goto done;

    desc.Width = WINEHUA_FEATURE_PROBE_SIZE;
    desc.Height = WINEHUA_FEATURE_PROBE_SIZE;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL |
                     D3D11_BIND_SHADER_RESOURCE;
    result = ID3D11Device_CreateTexture2D(
        state->device, &desc, NULL, &texture);
    dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateDepthStencilView(
            state->device, (ID3D11Resource *)texture,
            &dsv_desc, &write_dsv);
    dsv_desc.Flags = D3D11_DSV_READ_ONLY_DEPTH |
                     D3D11_DSV_READ_ONLY_STENCIL;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateDepthStencilView(
            state->device, (ID3D11Resource *)texture,
            &dsv_desc, &read_dsv);
    srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)texture, &srv_desc, &srv);

    depth_state_desc.DepthEnable = TRUE;
    depth_state_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth_state_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateDepthStencilState(
            state->device, &depth_state_desc, &read_depth_state);

    sampler_desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_LESS;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateSamplerState(
            state->device, &sampler_desc, &samplers[0]);
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateSamplerState(
            state->device, &sampler_desc, &samplers[1]);
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_GREATER;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateSamplerState(
            state->device, &sampler_desc, &samplers[2]);
    if (FAILED(result)) goto done;
    resources[0] = srv;

    ID3D11DeviceContext_ClearDepthStencilView(
        state->context, write_dsv,
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ID3D11DeviceContext_OMSetRenderTargets(
        state->context, 0, NULL, write_dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(state->context, NULL, 0);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(state->context, write_shader, NULL, 0);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_PSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);

    run_heaven_probe_pass(
        state, sample_shader, resources, 1, NULL, 0, NULL, NULL);
    if (!read_heaven_probe_points(
            state, depth_x, depth_y, WINEHUA_D24_DEPTH_POINTS,
            state->d24_initial_values, "D24 initial SRV"))
        goto done;
    for (i = 0; i < WINEHUA_D24_DEPTH_POINTS; ++i)
    {
        UINT value = state->d24_initial_values[i];
        UINT red = value & 0xffu;
        UINT green = (value >> 8) & 0xffu;
        UINT blue = (value >> 16) & 0xffu;
        if (red + 3 < initial_red[i] || red > initial_red[i] + 3 ||
            green != red || blue != red || (value >> 24) != 0xffu)
            state->d24_readonly_shadow_mismatches++;
    }

    run_heaven_probe_pass(
        state, compare_shader, resources, 1, samplers, 3,
        read_dsv, read_depth_state);
    if (!read_heaven_probe_points(
            state, compare_x, compare_y, WINEHUA_D24_COMPARE_POINTS,
            state->d24_compare_values, "D24 read-only SampleCmp"))
        goto done;
    for (i = 0; i < WINEHUA_D24_COMPARE_POINTS; ++i)
        if (state->d24_compare_values[i] != compare_expected[i])
            state->d24_readonly_shadow_mismatches++;

    ID3D11DeviceContext_ClearDepthStencilView(
        state->context, write_dsv,
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.9f, 0);
    ID3D11DeviceContext_OMSetRenderTargets(
        state->context, 0, NULL, write_dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(state->context, NULL, 0);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(state->context, rewrite_shader, NULL, 0);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_PSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);

    run_heaven_probe_pass(
        state, sample_shader, resources, 1, NULL, 0, NULL, NULL);
    if (!read_heaven_probe_points(
            state, depth_x, depth_y, WINEHUA_D24_DEPTH_POINTS,
            state->d24_rewrite_values, "D24 rewrite SRV"))
        goto done;
    for (i = 0; i < WINEHUA_D24_DEPTH_POINTS; ++i)
    {
        UINT value = state->d24_rewrite_values[i];
        UINT red = value & 0xffu;
        UINT green = (value >> 8) & 0xffu;
        UINT blue = (value >> 16) & 0xffu;
        if (red < 86 || red > 92 || green != red || blue != red ||
            (value >> 24) != 0xffu)
            state->d24_readonly_shadow_mismatches++;
    }
    state->d24_readonly_shadow_functional =
        state->d24_readonly_shadow_mismatches == 0;

done:
    if (FAILED(result))
    {
        state->present_result = result;
        fprintf(stderr,
                "winehua_d3d11_smoke: D24 read-only resource failure=0x%08lx\n",
                (unsigned long)result);
    }
    fprintf(stderr,
            "winehua_d3d11_smoke: D24 read-only mismatches=%u pass=%u\n",
            state->d24_readonly_shadow_mismatches,
            state->d24_readonly_shadow_functional);
    if (compare_shader) ID3D11PixelShader_Release(compare_shader);
    if (sample_shader) ID3D11PixelShader_Release(sample_shader);
    if (rewrite_shader) ID3D11PixelShader_Release(rewrite_shader);
    if (write_shader) ID3D11PixelShader_Release(write_shader);
    for (i = 0; i < 3; ++i)
        if (samplers[i]) ID3D11SamplerState_Release(samplers[i]);
    if (read_depth_state)
        ID3D11DepthStencilState_Release(read_depth_state);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (read_dsv) ID3D11DepthStencilView_Release(read_dsv);
    if (write_dsv) ID3D11DepthStencilView_Release(write_dsv);
    if (texture) ID3D11Texture2D_Release(texture);
    return state->d24_readonly_shadow_functional;
}

static BOOL run_rgba16f_rtv_srv_load_smoke(struct smoke_state *state)
{
    static const char *fill_source =
        "float stripeValue(float x) {"
        " if (x < 11.0) return -1.0; if (x < 22.0) return 0.25;"
        " if (x < 32.0) return 1.0; if (x < 43.0) return 2.0;"
        " if (x < 54.0) return 4.0; return 16.0; }"
        "float4 main(float4 pos : SV_Position) : SV_Target {"
        " float value = stripeValue(pos.x); return float4(value,value,value,1); }";
    static const char *validate_initial_source =
        "Texture2D<float4> inputTexture : register(t0);"
        "float stripeValue(float x) {"
        " if (x < 11.0) return -1.0; if (x < 22.0) return 0.25;"
        " if (x < 32.0) return 1.0; if (x < 43.0) return 2.0;"
        " if (x < 54.0) return 4.0; return 16.0; }"
        "float4 main(float4 pos : SV_Position) : SV_Target {"
        " float actual = inputTexture.Load(int3(pos.xy,0)).r;"
        " bool ok = abs(actual - stripeValue(pos.x)) < 0.06;"
        " return ok ? float4(0,1,0,1) : float4(1,0,0,1); }";
    static const char *add_source =
        "float4 main(float4 pos : SV_Position) : SV_Target {"
        " return float4(1,1,1,1); }";
    static const char *validate_added_source =
        "Texture2D<float4> inputTexture : register(t0);"
        "float stripeValue(float x) {"
        " if (x < 11.0) return -1.0; if (x < 22.0) return 0.25;"
        " if (x < 32.0) return 1.0; if (x < 43.0) return 2.0;"
        " if (x < 54.0) return 4.0; return 16.0; }"
        "float4 main(float4 pos : SV_Position) : SV_Target {"
        " float actual = inputTexture.Load(int3(pos.xy,0)).r;"
        " bool ok = abs(actual - (stripeValue(pos.x) + 1.0)) < 0.08;"
        " return ok ? float4(0,1,0,1) : float4(1,0,0,1); }";
    static const char *tonemap_source =
        "Texture2D<float4> inputTexture : register(t0);"
        "float4 main(float4 pos : SV_Position) : SV_Target {"
        " float value = max(inputTexture.Load(int3(pos.xy,0)).r, 0.0);"
        " float mapped = value / (1.0 + value);"
        " return float4(mapped,0,0,1); }";
    static const UINT x[WINEHUA_RGBA16F_POINTS] = {5, 16, 27, 37, 48, 58};
    static const UINT y[WINEHUA_RGBA16F_POINTS] = {32, 32, 32, 32, 32, 32};
    static const UINT tone_expected[WINEHUA_RGBA16F_POINTS] = {
        0, 142, 170, 191, 213, 241,
    };
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_BLEND_DESC blend_desc = {0};
    D3D11_VIEWPORT viewport = {0, 0, WINEHUA_FEATURE_PROBE_SIZE,
                               WINEHUA_FEATURE_PROBE_SIZE, 0.0f, 1.0f};
    ID3D11Texture2D *texture = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11BlendState *additive_blend = NULL;
    ID3D11PixelShader *fill_shader = NULL;
    ID3D11PixelShader *validate_initial_shader = NULL;
    ID3D11PixelShader *add_shader = NULL;
    ID3D11PixelShader *validate_added_shader = NULL;
    ID3D11PixelShader *tonemap_shader = NULL;
    ID3D11ShaderResourceView *resources[] = {NULL};
    ID3D11RenderTargetView *targets[] = {NULL};
    UINT i;
    HRESULT result = E_FAIL;

    state->rgba16f_rtv_srv_load_mismatches = 0;
    fill_shader = create_probe_pixel_shader(
        state, fill_source, "RGBA16F RTV fill");
    validate_initial_shader = create_probe_pixel_shader(
        state, validate_initial_source, "RGBA16F initial SRV");
    add_shader = create_probe_pixel_shader(
        state, add_source, "RGBA16F RTV LOAD blend");
    validate_added_shader = create_probe_pixel_shader(
        state, validate_added_source, "RGBA16F added SRV");
    tonemap_shader = create_probe_pixel_shader(
        state, tonemap_source, "RGBA16F tone map");
    if (!fill_shader || !validate_initial_shader || !add_shader ||
        !validate_added_shader || !tonemap_shader)
        goto done;

    desc.Width = WINEHUA_FEATURE_PROBE_SIZE;
    desc.Height = WINEHUA_FEATURE_PROBE_SIZE;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET |
                     D3D11_BIND_SHADER_RESOURCE;
    result = ID3D11Device_CreateTexture2D(
        state->device, &desc, NULL, &texture);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateRenderTargetView(
            state->device, (ID3D11Resource *)texture, NULL, &rtv);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)texture, NULL, &srv);

    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateBlendState(
            state->device, &blend_desc, &additive_blend);
    if (FAILED(result)) goto done;
    resources[0] = srv;
    targets[0] = rtv;

    ID3D11DeviceContext_OMSetRenderTargets(state->context, 1, targets, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(
        state->context, rtv, (const float[]){0, 0, 0, 1});
    ID3D11DeviceContext_OMSetBlendState(
        state->context, NULL, NULL, 0xffffffffu);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(state->context, fill_shader, NULL, 0);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_PSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);

    run_heaven_probe_pass(
        state, validate_initial_shader, resources, 1, NULL, 0, NULL, NULL);
    if (!read_heaven_probe_points(
            state, x, y, WINEHUA_RGBA16F_POINTS,
            state->rgba16f_initial_values, "RGBA16F initial SRV"))
        goto done;
    for (i = 0; i < WINEHUA_RGBA16F_POINTS; ++i)
        if (state->rgba16f_initial_values[i] != 0xff00ff00u)
            state->rgba16f_rtv_srv_load_mismatches++;

    /* The texture was just sampled as an SRV. Rebind it as an RTV without a
     * clear and blend +1 into every component. The destination term forces
     * the renderer to preserve and LOAD the previous FP16 attachment data. */
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 1, targets, NULL);
    ID3D11DeviceContext_OMSetBlendState(
        state->context, additive_blend, NULL, 0xffffffffu);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(state->context, add_shader, NULL, 0);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_PSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);
    ID3D11DeviceContext_OMSetBlendState(
        state->context, NULL, NULL, 0xffffffffu);

    run_heaven_probe_pass(
        state, validate_added_shader, resources, 1, NULL, 0, NULL, NULL);
    if (!read_heaven_probe_points(
            state, x, y, WINEHUA_RGBA16F_POINTS,
            state->rgba16f_added_values, "RGBA16F RTV LOAD blend SRV"))
        goto done;
    for (i = 0; i < WINEHUA_RGBA16F_POINTS; ++i)
        if (state->rgba16f_added_values[i] != 0xff00ff00u)
            state->rgba16f_rtv_srv_load_mismatches++;

    run_heaven_probe_pass(
        state, tonemap_shader, resources, 1, NULL, 0, NULL, NULL);
    if (!read_heaven_probe_points(
            state, x, y, WINEHUA_RGBA16F_POINTS,
            state->rgba16f_tonemap_values, "RGBA16F tone map"))
        goto done;
    for (i = 0; i < WINEHUA_RGBA16F_POINTS; ++i)
    {
        UINT value = state->rgba16f_tonemap_values[i];
        UINT red = value & 0xffu;
        UINT green = (value >> 8) & 0xffu;
        UINT blue = (value >> 16) & 0xffu;
        if (red + 4 < tone_expected[i] || red > tone_expected[i] + 4 ||
            green > 2 || blue > 2 || (value >> 24) != 0xffu)
            state->rgba16f_rtv_srv_load_mismatches++;
    }
    state->rgba16f_rtv_srv_load_functional =
        state->rgba16f_rtv_srv_load_mismatches == 0;

done:
    if (FAILED(result))
    {
        state->present_result = result;
        fprintf(stderr,
                "winehua_d3d11_smoke: RGBA16F resource failure=0x%08lx\n",
                (unsigned long)result);
    }
    fprintf(stderr,
            "winehua_d3d11_smoke: RGBA16F RTV-SRV-LOAD mismatches=%u pass=%u\n",
            state->rgba16f_rtv_srv_load_mismatches,
            state->rgba16f_rtv_srv_load_functional);
    if (tonemap_shader) ID3D11PixelShader_Release(tonemap_shader);
    if (validate_added_shader)
        ID3D11PixelShader_Release(validate_added_shader);
    if (add_shader) ID3D11PixelShader_Release(add_shader);
    if (validate_initial_shader)
        ID3D11PixelShader_Release(validate_initial_shader);
    if (fill_shader) ID3D11PixelShader_Release(fill_shader);
    if (additive_blend) ID3D11BlendState_Release(additive_blend);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (texture) ID3D11Texture2D_Release(texture);
    return state->rgba16f_rtv_srv_load_functional;
}

static void run_heaven_target_pass(
    struct smoke_state *state, ID3D11RenderTargetView *target,
    UINT width, UINT height, ID3D11PixelShader *pixel_shader,
    ID3D11ShaderResourceView *const *resources, UINT resource_count)
{
    D3D11_VIEWPORT viewport = {0, 0, width, height, 0.0f, 1.0f};
    ID3D11ShaderResourceView *null_resources[8] = {0};
    ID3D11RenderTargetView *targets[] = {target};

    ID3D11DeviceContext_OMSetRenderTargets(state->context, 1, targets, NULL);
    ID3D11DeviceContext_OMSetDepthStencilState(state->context, NULL, 0);
    ID3D11DeviceContext_OMSetBlendState(
        state->context, NULL, NULL, 0xffffffffu);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(
        state->context, pixel_shader, NULL, 0);
    if (resource_count)
        ID3D11DeviceContext_PSSetShaderResources(
            state->context, 0, resource_count, resources);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    if (resource_count)
        ID3D11DeviceContext_PSSetShaderResources(
            state->context, 0, resource_count, null_resources);
    ID3D11DeviceContext_PSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);
}

static BOOL run_heaven_mini_pipeline_smoke(struct smoke_state *state)
{
    static const char *geometry_source =
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "struct PSOut { float4 rt0 : SV_Target0; float4 rt1 : SV_Target1;"
        " float4 rt2 : SV_Target2; float depth : SV_Depth; };"
        "PSOut main(PSIn input) { PSOut output;"
        " bool right = input.pos.x >= 32.0;"
        " bool bottom = input.pos.y >= 32.0;"
        " float3 albedo = !bottom ? (right ? float3(0,1,0) : float3(1,0,0))"
        "                         : (right ? float3(1,1,1) : float3(0,0,1));"
        " float material = !bottom ? (right ? 0.5 : 0.25)"
        "                           : (right ? 1.0 : 0.75);"
        " float depth = !bottom ? (right ? 0.4 : 0.2)"
        "                        : (right ? 0.8 : 0.6);"
        " output.rt0 = float4(albedo,1);"
        " output.rt1 = float4(0.5,0.5,1,1);"
        " output.rt2 = float4(material,0,0,1);"
        " output.depth = depth; return output; }";
    static const char *lighting_source =
        "Texture2D<float4> gbuffer0 : register(t0);"
        "Texture2D<float4> gbuffer1 : register(t1);"
        "Texture2D<float4> gbuffer2 : register(t2);"
        "Texture2D<float> depthTexture : register(t3);"
        "float4 main(float4 pos : SV_Position) : SV_Target {"
        " int3 p = int3(pos.xy,0);"
        " float3 albedo = gbuffer0.Load(p).rgb;"
        " float ndotl = saturate(gbuffer1.Load(p).z * 2.0 - 1.0);"
        " float material = gbuffer2.Load(p).r;"
        " float depth = depthTexture.Load(p);"
        " float3 lit = albedo * (0.5 + material * 2.0) * ndotl"
        "            + depth.xxx * 0.25;"
        " return float4(lit,1); }";
    static const char *bloom_source =
        "Texture2D<float4> hdrTexture : register(t0);"
        "float4 main(float4 pos : SV_Position) : SV_Target {"
        " int2 p = int2(pos.xy) * 2;"
        " float3 value = (hdrTexture.Load(int3(p,0)).rgb"
        "              + hdrTexture.Load(int3(p + int2(1,0),0)).rgb"
        "              + hdrTexture.Load(int3(p + int2(0,1),0)).rgb"
        "              + hdrTexture.Load(int3(p + int2(1,1),0)).rgb) * 0.25;"
        " return float4(max(value - 1.0, 0.0),1); }";
    static const char *tonemap_source =
        "Texture2D<float4> hdrTexture : register(t0);"
        "Texture2D<float4> bloomTexture : register(t1);"
        "float4 main(float4 pos : SV_Position) : SV_Target {"
        " int2 p = int2(pos.xy);"
        " float3 hdr = hdrTexture.Load(int3(p,0)).rgb;"
        " float3 bloom = bloomTexture.Load(int3(p / 2,0)).rgb;"
        " float3 combined = hdr + bloom * 0.5;"
        " return float4(combined / (1.0 + combined),1); }";
    static const UINT x[WINEHUA_HEAVEN_MINI_POINTS] = {16, 48, 16, 48};
    static const UINT y[WINEHUA_HEAVEN_MINI_POINTS] = {16, 16, 48, 48};
    static const UINT expected[WINEHUA_HEAVEN_MINI_POINTS][3] = {
        {132, 12, 12}, {23, 167, 23}, {33, 33, 187}, {199, 199, 199},
    };
    D3D11_TEXTURE2D_DESC color_desc = {0};
    D3D11_TEXTURE2D_DESC depth_desc = {0};
    D3D11_TEXTURE2D_DESC fp16_desc = {0};
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
    D3D11_SHADER_RESOURCE_VIEW_DESC depth_srv_desc = {0};
    D3D11_VIEWPORT viewport = {0, 0, WINEHUA_FEATURE_PROBE_SIZE,
                               WINEHUA_FEATURE_PROBE_SIZE, 0.0f, 1.0f};
    ID3D11Texture2D *gbuffer[3] = {0};
    ID3D11RenderTargetView *gbuffer_rtvs[3] = {0};
    ID3D11ShaderResourceView *gbuffer_srvs[4] = {0};
    ID3D11ShaderResourceView *post_srvs[2] = {0};
    ID3D11Texture2D *depth = NULL;
    ID3D11DepthStencilView *depth_dsv = NULL;
    ID3D11Texture2D *hdr = NULL;
    ID3D11RenderTargetView *hdr_rtv = NULL;
    ID3D11ShaderResourceView *hdr_srv = NULL;
    ID3D11Texture2D *bloom = NULL;
    ID3D11RenderTargetView *bloom_rtv = NULL;
    ID3D11ShaderResourceView *bloom_srv = NULL;
    ID3D11PixelShader *geometry_shader = NULL;
    ID3D11PixelShader *lighting_shader = NULL;
    ID3D11PixelShader *bloom_shader = NULL;
    ID3D11PixelShader *tonemap_shader = NULL;
    UINT i;
    HRESULT result = E_FAIL;

    state->heaven_mini_pipeline_mismatches = 0;
    geometry_shader = create_probe_pixel_shader(
        state, geometry_source, "mini pipeline geometry");
    lighting_shader = create_probe_pixel_shader(
        state, lighting_source, "mini pipeline lighting");
    bloom_shader = create_probe_pixel_shader(
        state, bloom_source, "mini pipeline bloom downsample");
    tonemap_shader = create_probe_pixel_shader(
        state, tonemap_source, "mini pipeline tone map");
    if (!geometry_shader || !lighting_shader || !bloom_shader ||
        !tonemap_shader)
        goto done;

    color_desc.Width = WINEHUA_FEATURE_PROBE_SIZE;
    color_desc.Height = WINEHUA_FEATURE_PROBE_SIZE;
    color_desc.MipLevels = 1;
    color_desc.ArraySize = 1;
    color_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_desc.SampleDesc.Count = 1;
    color_desc.Usage = D3D11_USAGE_DEFAULT;
    color_desc.BindFlags = D3D11_BIND_RENDER_TARGET |
                           D3D11_BIND_SHADER_RESOURCE;
    result = S_OK;
    for (i = 0; i < 3 && SUCCEEDED(result); ++i)
    {
        result = ID3D11Device_CreateTexture2D(
            state->device, &color_desc, NULL, &gbuffer[i]);
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateRenderTargetView(
                state->device, (ID3D11Resource *)gbuffer[i], NULL,
                &gbuffer_rtvs[i]);
        if (SUCCEEDED(result))
            result = ID3D11Device_CreateShaderResourceView(
                state->device, (ID3D11Resource *)gbuffer[i], NULL,
                &gbuffer_srvs[i]);
    }

    depth_desc = color_desc;
    depth_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL |
                           D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateTexture2D(
            state->device, &depth_desc, NULL, &depth);
    dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateDepthStencilView(
            state->device, (ID3D11Resource *)depth, &dsv_desc, &depth_dsv);
    depth_srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depth_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depth_srv_desc.Texture2D.MipLevels = 1;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)depth, &depth_srv_desc,
            &gbuffer_srvs[3]);

    fp16_desc = color_desc;
    fp16_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateTexture2D(
            state->device, &fp16_desc, NULL, &hdr);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateRenderTargetView(
            state->device, (ID3D11Resource *)hdr, NULL, &hdr_rtv);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)hdr, NULL, &hdr_srv);
    fp16_desc.Width /= 2;
    fp16_desc.Height /= 2;
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateTexture2D(
            state->device, &fp16_desc, NULL, &bloom);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateRenderTargetView(
            state->device, (ID3D11Resource *)bloom, NULL, &bloom_rtv);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateShaderResourceView(
            state->device, (ID3D11Resource *)bloom, NULL, &bloom_srv);
    if (FAILED(result)) goto done;

    ID3D11DeviceContext_OMSetRenderTargets(
        state->context, 3, gbuffer_rtvs, depth_dsv);
    for (i = 0; i < 3; ++i)
        ID3D11DeviceContext_ClearRenderTargetView(
            state->context, gbuffer_rtvs[i], (const float[]){0, 0, 0, 1});
    ID3D11DeviceContext_ClearDepthStencilView(
        state->context, depth_dsv,
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ID3D11DeviceContext_OMSetDepthStencilState(state->context, NULL, 0);
    ID3D11DeviceContext_OMSetBlendState(
        state->context, NULL, NULL, 0xffffffffu);
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(state->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(
        state->context, state->fullscreen_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(
        state->context, geometry_shader, NULL, 0);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_PSSetShader(state->context, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);

    ID3D11DeviceContext_ClearRenderTargetView(
        state->context, hdr_rtv, (const float[]){0, 0, 0, 1});
    run_heaven_target_pass(
        state, hdr_rtv, WINEHUA_FEATURE_PROBE_SIZE,
        WINEHUA_FEATURE_PROBE_SIZE, lighting_shader, gbuffer_srvs, 4);

    post_srvs[0] = hdr_srv;
    ID3D11DeviceContext_ClearRenderTargetView(
        state->context, bloom_rtv, (const float[]){0, 0, 0, 1});
    run_heaven_target_pass(
        state, bloom_rtv, WINEHUA_FEATURE_PROBE_SIZE / 2,
        WINEHUA_FEATURE_PROBE_SIZE / 2, bloom_shader, post_srvs, 1);

    post_srvs[1] = bloom_srv;
    run_heaven_target_pass(
        state, state->probe_rtv, WINEHUA_FEATURE_PROBE_SIZE,
        WINEHUA_FEATURE_PROBE_SIZE, tonemap_shader, post_srvs, 2);
    if (!read_heaven_probe_points(
            state, x, y, WINEHUA_HEAVEN_MINI_POINTS,
            state->heaven_mini_pipeline_values, "Heaven mini pipeline"))
        goto done;
    for (i = 0; i < WINEHUA_HEAVEN_MINI_POINTS; ++i)
    {
        UINT value = state->heaven_mini_pipeline_values[i];
        UINT red = value & 0xffu;
        UINT green = (value >> 8) & 0xffu;
        UINT blue = (value >> 16) & 0xffu;
        if (red + 6 < expected[i][0] || red > expected[i][0] + 6 ||
            green + 6 < expected[i][1] || green > expected[i][1] + 6 ||
            blue + 6 < expected[i][2] || blue > expected[i][2] + 6 ||
            (value >> 24) != 0xffu)
            state->heaven_mini_pipeline_mismatches++;
    }
    state->heaven_mini_pipeline_functional =
        state->heaven_mini_pipeline_mismatches == 0;

done:
    if (FAILED(result))
    {
        state->present_result = result;
        fprintf(stderr,
                "winehua_d3d11_smoke: Heaven mini pipeline resource failure=0x%08lx\n",
                (unsigned long)result);
    }
    fprintf(stderr,
            "winehua_d3d11_smoke: Heaven mini pipeline mismatches=%u pass=%u\n",
            state->heaven_mini_pipeline_mismatches,
            state->heaven_mini_pipeline_functional);
    if (tonemap_shader) ID3D11PixelShader_Release(tonemap_shader);
    if (bloom_shader) ID3D11PixelShader_Release(bloom_shader);
    if (lighting_shader) ID3D11PixelShader_Release(lighting_shader);
    if (geometry_shader) ID3D11PixelShader_Release(geometry_shader);
    if (bloom_srv) ID3D11ShaderResourceView_Release(bloom_srv);
    if (bloom_rtv) ID3D11RenderTargetView_Release(bloom_rtv);
    if (bloom) ID3D11Texture2D_Release(bloom);
    if (hdr_srv) ID3D11ShaderResourceView_Release(hdr_srv);
    if (hdr_rtv) ID3D11RenderTargetView_Release(hdr_rtv);
    if (hdr) ID3D11Texture2D_Release(hdr);
    if (gbuffer_srvs[3])
        ID3D11ShaderResourceView_Release(gbuffer_srvs[3]);
    if (depth_dsv) ID3D11DepthStencilView_Release(depth_dsv);
    if (depth) ID3D11Texture2D_Release(depth);
    for (i = 0; i < 3; ++i)
    {
        if (gbuffer_srvs[i])
            ID3D11ShaderResourceView_Release(gbuffer_srvs[i]);
        if (gbuffer_rtvs[i])
            ID3D11RenderTargetView_Release(gbuffer_rtvs[i]);
        if (gbuffer[i]) ID3D11Texture2D_Release(gbuffer[i]);
    }
    return state->heaven_mini_pipeline_functional;
}

static BOOL run_heaven_pass_probes(struct smoke_state *state)
{
    const BOOL mrt = run_mrt_gbuffer_smoke(state);
    const BOOL d24 = run_d24_readonly_shadow_smoke(state);
    const BOOL rgba16f = run_rgba16f_rtv_srv_load_smoke(state);
    const BOOL mini = run_heaven_mini_pipeline_smoke(state);

    state->heaven_pass_functional = mrt && d24 && rgba16f && mini;
    fprintf(stderr,
            "winehua_d3d11_smoke: Heaven pass matrix MRT=%u D24=%u RGBA16F=%u mini=%u pass=%u\n",
            mrt, d24, rgba16f, mini, state->heaven_pass_functional);
    return state->heaven_pass_functional;
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
    state->rgba_load_ps_functional = run_texture_ps_probe(
        state, state->rgba_srv, state->rgba_load_pixel_shader, NULL, "Load",
        WINEHUA_RGBA_EXPECTED, &state->rgba_load_ps_value);
    state->rgba_load_cs_functional = run_rgba_cs_probe(
        state, state->rgba_srv, state->rgba_load_compute_shader, NULL, "Load",
        &state->rgba_load_cs_value);
    state->rgba_point_ps_functional = run_texture_ps_probe(
        state, state->rgba_srv, state->rgba_sample_pixel_shader,
        state->point_sampler, "POINT", WINEHUA_RGBA_EXPECTED,
        &state->rgba_point_ps_value);
    state->rgba_point_cs_functional = run_rgba_cs_probe(
        state, state->rgba_srv, state->rgba_sample_compute_shader,
        state->point_sampler, "POINT",
        &state->rgba_point_cs_value);
    state->rgba_linear_ps_functional = run_texture_ps_probe(
        state, state->rgba_srv, state->rgba_sample_pixel_shader,
        state->linear_sampler, "LINEAR", WINEHUA_RGBA_EXPECTED,
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
    state->rgba_updated_load_ps_functional = run_texture_ps_probe(
        state, state->rgba_updated_srv, state->rgba_load_pixel_shader,
        NULL, "UPDATED Load", WINEHUA_RGBA_EXPECTED,
        &state->rgba_updated_load_ps_value);
    state->rgba_updated_load_cs_functional = run_rgba_cs_probe(
        state, state->rgba_updated_srv, state->rgba_load_compute_shader,
        NULL, "UPDATED Load", &state->rgba_updated_load_cs_value);
    state->rgba_updated_point_ps_functional = run_texture_ps_probe(
        state, state->rgba_updated_srv, state->rgba_sample_pixel_shader,
        state->point_sampler, "UPDATED POINT", WINEHUA_RGBA_EXPECTED,
        &state->rgba_updated_point_ps_value);
    state->rgba_updated_point_cs_functional = run_rgba_cs_probe(
        state, state->rgba_updated_srv, state->rgba_sample_compute_shader,
        state->point_sampler, "UPDATED POINT", &state->rgba_updated_point_cs_value);

    /* Four distinct immutable textures make descriptor slot permutation
     * visible in one readback.  The following passes also exercise immediate
     * context dirty-state updates, an explicitly unbound slot, and releasing
     * the application's references immediately after binding. */
    if (!run_descriptor_identity_probes(state))
        fprintf(stderr, "winehua_d3d11_smoke: descriptor identity matrix failed\n");
    if (!run_sampler_pair_probe(state))
        fprintf(stderr, "winehua_d3d11_smoke: shared sampler-slot matrix failed\n");
    if (!run_subresource_probes(state))
        fprintf(stderr, "winehua_d3d11_smoke: subresource matrix failed\n");
    if (!run_texture3d_probes(state))
        fprintf(stderr, "winehua_d3d11_smoke: Texture3D ping-pong matrix failed\n");
    if (!run_texture3d_boundary_probes(state))
        fprintf(stderr, "winehua_d3d11_smoke: Texture3D boundary matrix failed\n");

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
    if (!run_bc_matrix_probes(state))
        fprintf(stderr, "winehua_d3d11_smoke: BC format/mip matrix failed\n");

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
           state->bc_matrix_functional &&
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
           state->sampler_pair_functional &&
           state->subresource_functional &&
           state->texture3d_created &&
           state->texture3d_upload_functional &&
           state->texture3d_single_dispatch_functional &&
           state->texture3d_uav_to_srv_functional &&
           state->texture3d_pingpong_functional &&
           state->texture3d_oob_load_functional &&
           state->texture3d_oob_index_functional &&
           state->texture3d_border_point_functional &&
           state->texture3d_border_linear_functional &&
           state->heaven_resource_functional &&
           state->heaven_pass_functional &&
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
    if (!create_bc_matrix_resources(state)) return FALSE;
    if (!create_pattern_texture(state)) return FALSE;
    if (!create_rgba_sample_texture(state)) return FALSE;
    if (!create_descriptor_identity_resources(state)) return FALSE;
    if (!create_pipeline_states(state)) return FALSE;

    if (!create_feature_shaders(state) ||
        !create_feature_resources(state) ||
        !run_feature_probes(state) ||
        !run_heaven_resource_probes(state) ||
        !run_heaven_pass_probes(state))
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
    if (state->texture3d_staging)
        ID3D11Texture3D_Release(state->texture3d_staging);
    if (state->texture3d_border_srv)
        ID3D11ShaderResourceView_Release(state->texture3d_border_srv);
    if (state->texture3d_border)
        ID3D11Texture3D_Release(state->texture3d_border);
    for (i = 0; i < 2; ++i)
    {
        if (state->texture3d_uavs[i])
            ID3D11UnorderedAccessView_Release(state->texture3d_uavs[i]);
        if (state->texture3d_srvs[i])
            ID3D11ShaderResourceView_Release(state->texture3d_srvs[i]);
        if (state->texture3d[i]) ID3D11Texture3D_Release(state->texture3d[i]);
    }
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
    if (state->sampler_pair_pixel_shader)
        ID3D11PixelShader_Release(state->sampler_pair_pixel_shader);
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
    if (state->texture3d_pingpong_compute_shader)
        ID3D11ComputeShader_Release(state->texture3d_pingpong_compute_shader);
    if (state->texture3d_oob_compute_shader)
        ID3D11ComputeShader_Release(state->texture3d_oob_compute_shader);
    if (state->texture3d_border_compute_shader)
        ID3D11ComputeShader_Release(state->texture3d_border_compute_shader);
    if (state->constant_buffer) ID3D11Buffer_Release(state->constant_buffer);
    if (state->dynamic_constant_staging)
        ID3D11Buffer_Release(state->dynamic_constant_staging);
    if (state->index_buffer) ID3D11Buffer_Release(state->index_buffer);
    if (state->vertex_buffer) ID3D11Buffer_Release(state->vertex_buffer);
    if (state->input_layout) ID3D11InputLayout_Release(state->input_layout);
    if (state->vertex_shader) ID3D11VertexShader_Release(state->vertex_shader);
    if (state->pixel_shader) ID3D11PixelShader_Release(state->pixel_shader);
    if (state->bc_sampler) ID3D11SamplerState_Release(state->bc_sampler);
    if (state->bc_mip_sampler)
        ID3D11SamplerState_Release(state->bc_mip_sampler);
    if (state->point_sampler) ID3D11SamplerState_Release(state->point_sampler);
    if (state->linear_sampler) ID3D11SamplerState_Release(state->linear_sampler);
    if (state->border_point_sampler)
        ID3D11SamplerState_Release(state->border_point_sampler);
    if (state->border_linear_sampler)
        ID3D11SamplerState_Release(state->border_linear_sampler);
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
    for (i = 0; i < WINEHUA_BC_MATRIX_FORMATS; ++i)
    {
        if (state->bc_matrix_srvs[i])
            ID3D11ShaderResourceView_Release(state->bc_matrix_srvs[i]);
        if (state->bc_matrix_textures[i])
            ID3D11Texture2D_Release(state->bc_matrix_textures[i]);
    }
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
    BOOL long_run;
    ULONGLONG long_deadline_ms = 0;
    ULONGLONG next_heartbeat_ms = 0;
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
        write_state(&state, "FAIL", "dxvk",
                    "D3D11 initialization or required feature contract failed");
        release_state(&state);
        return 1;
    }
    long_run = state.smoke.automation && state.smoke.seconds >= 60;
    frame_target = state.smoke.seconds ? state.smoke.seconds * 30 : 30;
    if (long_run)
    {
        long_deadline_ms = GetTickCount64() + (ULONGLONG)state.smoke.seconds * 1000;
        next_heartbeat_ms = GetTickCount64() + 5000;
    }
    while (long_run ? GetTickCount64() < long_deadline_ms
                    : state.frame_count < frame_target)
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
            if (state.smoke.automation && !long_run) break;
        }
        if (long_run && GetTickCount64() >= next_heartbeat_ms)
        {
            write_state(&state, "RUNNING", "dxvk", "long-running");
            next_heartbeat_ms = GetTickCount64() + 5000;
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
    write_state(&state, "PASS", "dxvk", long_run
                ? "DXVK Legacy D3D11 long-run check passed"
                : "DXVK Legacy D3D11 fixed-frame check passed");
    release_state(&state);
    return 0;
}
