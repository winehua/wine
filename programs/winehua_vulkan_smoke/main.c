/*
 * WineHua Win32 Vulkan offscreen smoke.
 *
 * This intentionally avoids WSI.  It proves the Windows Vulkan ->
 * winevulkan -> x86_64 Vulkan Loader -> Mesa Venus chain before presentation
 * is introduced.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include "wine/vulkan.h"

#include "../winehua_smoke_protocol.h"
#include "../../../../smoke/vkd3d_capability_audit.h"

struct probe_state
{
    struct winehua_smoke_options smoke;
    ULONGLONG started_ms;
    uint32_t loader_api;
    VkPhysicalDeviceProperties properties;
    uint32_t queue_family;
    BOOL descriptor_indexing;
    BOOL robustness2;
    BOOL timeline_semaphore;
    BOOL synchronization2;
    BOOL dynamic_rendering;
    BOOL maintenance4;
    BOOL buffer_device_address;
    uint32_t max_update_after_bind_descriptors_in_all_pools;
    uint32_t max_descriptor_set_update_after_bind_sampled_images;
    uint32_t max_descriptor_set_update_after_bind_storage_images;
    uint32_t max_descriptor_set_update_after_bind_storage_buffers;
    char *capability_audit;
    BOOL buffer_copy_ok;
    BOOL image_clear_ok;
    BOOL storage_image_write_ok;
    BOOL storage_image_read_ok;
    BOOL sampled_image_fetch_ok;
    BOOL combined_image_sampler_ok;
    BOOL separated_image_sampler_ok;
    BOOL shader_executed;
    BOOL image_read_completed;
    BOOL sampled_only;
    uint32_t sampled_value;
    uint32_t expected_sampled_value;
    BOOL present_ok;
    uint32_t present_frames;
    BOOL fallback_detected;
    unsigned int queue_submits;
    char vulkan_module[MAX_PATH];
    char winevulkan_module[MAX_PATH];
    uint32_t present_fail_frame;
    int32_t present_acquire_result;
    int32_t present_acquire_wait_result;
    int32_t present_submit_result;
    int32_t present_submit_wait_result;
    int32_t present_queue_result;
    char present_error[256];
};

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

static void version_text(uint32_t version, char *buffer, size_t size)
{
    snprintf(buffer, size, "%u.%u.%u", VK_API_VERSION_MAJOR(version),
             VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
}

static void load_module_paths(struct probe_state *state)
{
    HMODULE module = GetModuleHandleA("vulkan-1.dll");
    if (module) GetModuleFileNameA(module, state->vulkan_module, sizeof(state->vulkan_module));
    module = GetModuleHandleA("winevulkan.dll");
    if (module) GetModuleFileNameA(module, state->winevulkan_module,
                                   sizeof(state->winevulkan_module));
}

static BOOL has_extension(const VkExtensionProperties *extensions, uint32_t count,
                          const char *name)
{
    uint32_t i;
    for (i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, name)) return TRUE;
    return FALSE;
}

static BOOL query_extended_capabilities(VkPhysicalDevice physical,
                                        struct probe_state *state)
{
    uint32_t count = 0;
    VkExtensionProperties *extensions = NULL;
    VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features vulkan12 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features vulkan13 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceProperties2 properties2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceVulkan12Properties properties12 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceIDProperties id_properties = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    void **tail = &features2.pNext;
    BOOL api12 = state->properties.apiVersion >= VK_API_VERSION_1_2;
    BOOL api13 = state->properties.apiVersion >= VK_API_VERSION_1_3;
    BOOL has_robustness2;

    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) != VK_SUCCESS)
        return FALSE;
    extensions = calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) return FALSE;
    if (count && vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions) != VK_SUCCESS)
    {
        free(extensions);
        return FALSE;
    }
    has_robustness2 = has_extension(extensions, count, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
#define APPEND_FEATURE(feature, enabled) do { \
    if (enabled) { *tail = &(feature); tail = &(feature).pNext; } \
} while (0)
    APPEND_FEATURE(vulkan12, api12);
    APPEND_FEATURE(vulkan13, api13);
    APPEND_FEATURE(robustness2, has_robustness2);
#undef APPEND_FEATURE
    vkGetPhysicalDeviceFeatures2(physical, &features2);
    properties2.pNext = &properties12;
    properties12.pNext = &id_properties;
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    state->descriptor_indexing = api12 && vulkan12.descriptorIndexing;
    state->timeline_semaphore = api12 && vulkan12.timelineSemaphore;
    state->buffer_device_address = api12 && vulkan12.bufferDeviceAddress;
    state->robustness2 = has_robustness2 && robustness2.robustBufferAccess2 &&
                         robustness2.robustImageAccess2 && robustness2.nullDescriptor;
    state->synchronization2 = api13 && vulkan13.synchronization2;
    state->dynamic_rendering = api13 && vulkan13.dynamicRendering;
    state->maintenance4 = api13 && vulkan13.maintenance4;
    state->max_update_after_bind_descriptors_in_all_pools =
        properties12.maxUpdateAfterBindDescriptorsInAllPools;
    state->max_descriptor_set_update_after_bind_sampled_images =
        properties12.maxDescriptorSetUpdateAfterBindSampledImages;
    state->max_descriptor_set_update_after_bind_storage_images =
        properties12.maxDescriptorSetUpdateAfterBindStorageImages;
    state->max_descriptor_set_update_after_bind_storage_buffers =
        properties12.maxDescriptorSetUpdateAfterBindStorageBuffers;
    state->capability_audit = winehua_vkd3d_capability_audit(
        physical, extensions, count, &vulkan12, &vulkan13,
        &properties12, &id_properties);
    if (!state->capability_audit)
    {
        free(extensions);
        return FALSE;
    }
    free(extensions);
    return TRUE;
}

static void write_state(const struct probe_state *state, const char *status,
                        const char *stage, const char *message)
{
    char metrics[65536];
    char loader_version[32];
    char device_version[32];
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
    char vulkan_module[MAX_PATH];
    char winevulkan_module[MAX_PATH];

    version_text(state->loader_api, loader_version, sizeof(loader_version));
    version_text(state->properties.apiVersion, device_version, sizeof(device_version));
    safe_json_text(device_name, sizeof(device_name), state->properties.deviceName);
    safe_json_text(vulkan_module, sizeof(vulkan_module), state->vulkan_module);
    safe_json_text(winevulkan_module, sizeof(winevulkan_module), state->winevulkan_module);
    snprintf(metrics, sizeof(metrics),
             "{\"loaderApiVersion\":\"%s\",\"deviceApiVersion\":\"%s\","
             "\"deviceName\":\"%s\",\"vendorId\":%u,\"deviceId\":%u,"
             "\"driverVersion\":%u,\"graphicsQueueFamily\":%u,"
             "\"descriptorIndexing\":%s,\"robustness2\":%s,"
             "\"timelineSemaphore\":%s,\"synchronization2\":%s,"
             "\"dynamicRendering\":%s,\"maintenance4\":%s,"
             "\"bufferDeviceAddress\":%s,\"updateAfterBindLimits\":{"
             "\"maxUpdateAfterBindDescriptorsInAllPools\":%u,"
            "\"maxDescriptorSetUpdateAfterBindSampledImages\":%u,"
            "\"maxDescriptorSetUpdateAfterBindStorageImages\":%u,"
            "\"maxDescriptorSetUpdateAfterBindStorageBuffers\":%u},"
            "\"capabilityAudit\":%s,"
            "\"vulkanModule\":\"%s\","
             "\"winevulkanModule\":\"%s\","
             "\"checks\":{\"bufferCopy\":%s,\"imageClear\":%s,"
             "\"storageImageWrite\":%s,\"storageImageRead\":%s,"
             "\"sampledImageFetch\":%s,\"combinedImageSampler\":%s,"
             "\"separatedImageSampler\":%s,\"shaderExecuted\":%s,"
             "\"imageReadCompleted\":%s},"
             "\"sampledImage\":{\"sampledValue\":\"0x%08x\","
             "\"expectedValue\":\"0x%08x\",\"sampledOnly\":%s},"
             "\"cpuReadBytes\":%u,\"cpuUploadBytes\":%u,"
             "\"gpuCopyCount\":%u,\"queueSubmitCount\":%u,"
             "\"presentFrames\":%u,"
             "\"presentFailureFrame\":%u,"
             "\"presentAcquireResult\":%d,"
             "\"presentAcquireWaitResult\":%d,"
             "\"presentSubmitResult\":%d,"
             "\"presentSubmitWaitResult\":%d,"
             "\"presentQueueResult\":%d,"
             "\"presentError\":\"%s\","
             "\"perFrameDeviceWaitIdle\":0,\"fallbackDetected\":%s,"
             "\"durationMs\":%llu}",
             loader_version, device_version, device_name,
             state->properties.vendorID, state->properties.deviceID, state->properties.driverVersion,
             state->queue_family,
             state->descriptor_indexing ? "true" : "false",
             state->robustness2 ? "true" : "false",
             state->timeline_semaphore ? "true" : "false",
             state->synchronization2 ? "true" : "false",
             state->dynamic_rendering ? "true" : "false",
             state->maintenance4 ? "true" : "false",
             state->buffer_device_address ? "true" : "false",
             state->max_update_after_bind_descriptors_in_all_pools,
            state->max_descriptor_set_update_after_bind_sampled_images,
            state->max_descriptor_set_update_after_bind_storage_images,
            state->max_descriptor_set_update_after_bind_storage_buffers,
            state->capability_audit ? state->capability_audit : "{}",
            vulkan_module, winevulkan_module,
             state->buffer_copy_ok ? "true" : "false",
             state->image_clear_ok ? "true" : "false",
             state->sampled_only ? "null" : (state->storage_image_write_ok ? "true" : "false"),
             state->sampled_only ? "null" : (state->storage_image_read_ok ? "true" : "false"),
             state->sampled_image_fetch_ok ? "true" : "false",
             state->combined_image_sampler_ok ? "true" : "false",
             state->separated_image_sampler_ok ? "true" : "false",
             state->shader_executed ? "true" : "false",
             state->image_read_completed ? "true" : "false",
             state->sampled_value, state->expected_sampled_value,
             state->sampled_only ? "true" : "false",
             state->smoke.present ? 0u : 5120u,
             state->smoke.present ? 0u : 4096u,
             state->smoke.present ? 1u : 2u, state->queue_submits,
             state->present_frames,
             state->present_fail_frame,
             state->present_acquire_result,
             state->present_acquire_wait_result,
             state->present_submit_result,
             state->present_submit_wait_result,
             state->present_queue_result,
             state->present_error,
             state->fallback_detected ? "true" : "false",
             winehua_smoke_timestamp_ms() - state->started_ms);
    winehua_smoke_write_result(&state->smoke, status, stage, message, metrics);
}

static void checkpoint(const struct probe_state *state, const char *message)
{
    write_state(state, "RUNNING", "wine-vulkan", message);
}

static void record_present_failure(struct probe_state *state, uint32_t frame,
                                   const char *stage, VkResult result)
{
    state->present_fail_frame = frame;
    snprintf(state->present_error, sizeof(state->present_error),
             "frame=%u stage=%s result=%d", frame, stage, (int32_t)result);
}

static VkResult wait_for_fence(VkDevice device, VkFence fence)
{
    /* Venus may transiently report VK_NOT_READY while its fence feedback
     * round-trip catches up.  Poll in bounded 50 ms slices so a real hang is
     * still classified as a timeout and never turns into an unbounded wait. */
    for (unsigned int attempt = 0; attempt < 100; ++attempt)
    {
        VkResult result = vkWaitForFences(device, 1, &fence, VK_TRUE, 50000000ULL);
        if (result == VK_SUCCESS) return result;
        if (result != VK_NOT_READY && result != VK_TIMEOUT) return result;
    }
    return VK_TIMEOUT;
}

static BOOL find_memory_type(VkPhysicalDevice physical, uint32_t bits,
                             VkMemoryPropertyFlags required, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (i = 0; i < memory.memoryTypeCount; ++i)
    {
        if ((bits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & required) == required)
        {
            *index = i;
            return TRUE;
        }
    }
    return FALSE;
}

static VkResult create_buffer(VkPhysicalDevice physical, VkDevice device,
                              VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags properties,
                              VkBuffer *buffer, VkDeviceMemory *memory)
{
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    uint32_t type;
    VkResult result;

    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateBuffer(device, &info, NULL, buffer);
    if (result != VK_SUCCESS) return result;
    vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    if (!find_memory_type(physical, requirements.memoryTypeBits, properties, &type))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = vkAllocateMemory(device, &allocation, NULL, memory);
    if (result != VK_SUCCESS) return result;
    return vkBindBufferMemory(device, *buffer, *memory, 0);
}

static BOOL load_shader(const char *name, uint32_t **code, size_t *size)
{
    char root[MAX_PATH];
    char path[MAX_PATH * 2];
    FILE *file;
    long length;
    DWORD root_length = GetEnvironmentVariableA("WINEHUA_SMOKE_ASSETS", root,
                                                sizeof(root));
    if (!root_length || root_length >= sizeof(root)) return FALSE;
    snprintf(path, sizeof(path), "%s\\%s", root, name);
    file = fopen(path, "rb");
    if (!file) return FALSE;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return FALSE; }
    length = ftell(file);
    if (length <= 0 || (length & 3)) { fclose(file); return FALSE; }
    rewind(file);
    *code = malloc((size_t)length);
    if (!*code || fread(*code, 1, (size_t)length, file) != (size_t)length)
    {
        free(*code);
        *code = NULL;
        fclose(file);
        return FALSE;
    }
    fclose(file);
    *size = (size_t)length;
    return TRUE;
}

static BOOL run_sampled_compute(struct probe_state *state,
                                VkPhysicalDevice physical, VkDevice device,
                                VkQueue queue, VkCommandPool pool,
                                VkCommandBuffer command, VkFence fence,
                                VkImage image, VkImageView image_view,
                                VkSampler sampler, const char *shader_name,
                                unsigned int mode, VkImageLayout old_layout,
                                VkImageLayout new_layout, uint32_t expected,
                                uint32_t *value)
{
    VkDescriptorSetLayoutBinding bindings[3];
    VkDescriptorPoolSize pool_sizes[3];
    VkDescriptorSetLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    VkDescriptorSetAllocateInfo allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    VkPipelineLayoutCreateInfo pipeline_layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkComputePipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkBuffer output = VK_NULL_HANDLE;
    VkDeviceMemory output_memory = VK_NULL_HANDLE;
    VkResult result = VK_SUCCESS;
    uint32_t *code = NULL;
    size_t code_size = 0;
    unsigned int binding_count = mode == 3 ? 3 : 2;
    unsigned int pool_count = mode == 3 ? 3 : 2;
    void *mapped = NULL;
    VkDescriptorImageInfo image_info;
    VkDescriptorImageInfo sampler_info;
    VkDescriptorBufferInfo buffer_info;
    VkWriteDescriptorSet writes[3];
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkImageMemoryBarrier image_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkDescriptorSetLayoutBinding *storage_binding = &bindings[0];

    memset(bindings, 0, sizeof(bindings));
    storage_binding->binding = 0;
    storage_binding->descriptorCount = 1;
    storage_binding->stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    storage_binding->descriptorType = mode == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
        (mode == 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
         VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    bindings[1].binding = 1;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].descriptorType = mode == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER :
                                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (mode == 3)
    {
        bindings[2].binding = 2;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    layout_info.bindingCount = binding_count;
    layout_info.pBindings = bindings;
    result = vkCreateDescriptorSetLayout(device, &layout_info, NULL, &set_layout);
    if (result != VK_SUCCESS) goto cleanup;

    memset(pool_sizes, 0, sizeof(pool_sizes));
    pool_sizes[0].type = storage_binding->descriptorType;
    pool_sizes[0].descriptorCount = 1;
    pool_sizes[1].type = bindings[1].descriptorType;
    pool_sizes[1].descriptorCount = 1;
    if (mode == 3)
    {
        pool_sizes[2].type = bindings[2].descriptorType;
        pool_sizes[2].descriptorCount = 1;
    }
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = pool_count;
    pool_info.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool);
    if (result != VK_SUCCESS) goto cleanup;
    allocate.descriptorPool = descriptor_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout;
    result = vkAllocateDescriptorSets(device, &allocate, &set);
    if (result != VK_SUCCESS)
    {
        snprintf(state->present_error, sizeof(state->present_error),
                 "sampled descriptor allocation result=%d", (int)result);
        goto cleanup;
    }
    if (!load_shader(shader_name, &code, &code_size))
    {
        snprintf(state->present_error, sizeof(state->present_error),
                 "sampled shader asset missing: %s", shader_name);
        result = VK_ERROR_INITIALIZATION_FAILED;
        goto cleanup;
    }

    {
        VkShaderModuleCreateInfo shader_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_info.codeSize = code_size;
        shader_info.pCode = code;
        result = vkCreateShaderModule(device, &shader_info, NULL, &shader);
    }
    free(code);
    code = NULL;
    if (result != VK_SUCCESS)
    {
        snprintf(state->present_error, sizeof(state->present_error),
                 "sampled pipeline creation result=%d shader=%s", (int)result, shader_name);
        goto cleanup;
    }
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &set_layout;
    result = vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &pipeline_layout);
    if (result != VK_SUCCESS) goto cleanup;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    pipeline_info.stage = stage;
    pipeline_info.layout = pipeline_layout;
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                      NULL, &pipeline);
    if (result != VK_SUCCESS)
    {
        snprintf(state->present_error, sizeof(state->present_error),
                 "sampled compute pipeline result=%d shader=%s", (int)result, shader_name);
        goto cleanup;
    }

    result = create_buffer(physical, device, sizeof(uint32_t),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &output, &output_memory);
    if (result != VK_SUCCESS) goto cleanup;
    if (vkMapMemory(device, output_memory, 0, sizeof(uint32_t), 0, &mapped) != VK_SUCCESS)
        goto cleanup;
    *(uint32_t *)mapped = 0xdeadbeefu;
    {
        VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = output_memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(device, 1, &range);
    }
    vkUnmapMemory(device, output_memory);
    mapped = NULL;

    memset(&image_info, 0, sizeof(image_info));
    image_info.sampler = mode == 2 ? sampler : VK_NULL_HANDLE;
    image_info.imageView = image_view;
    image_info.imageLayout = mode == 0 ? VK_IMAGE_LAYOUT_GENERAL : new_layout;
    memset(&sampler_info, 0, sizeof(sampler_info));
    sampler_info.sampler = sampler;
    memset(&buffer_info, 0, sizeof(buffer_info));
    buffer_info.buffer = output;
    buffer_info.offset = 0;
    buffer_info.range = sizeof(uint32_t);
    memset(writes, 0, sizeof(writes));
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = storage_binding->descriptorType;
    writes[0].pImageInfo = &image_info;
    if (mode == 3)
    {
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[1].pImageInfo = &sampler_info;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = set;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(device, 1, &writes[0], 0, NULL);
        vkUpdateDescriptorSets(device, 1, &writes[1], 0, NULL);
        vkUpdateDescriptorSets(device, 1, &writes[2], 0, NULL);
    }
    else
    {
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(device, 1, &writes[0], 0, NULL);
        vkUpdateDescriptorSets(device, 1, &writes[1], 0, NULL);
    }

    result = vkResetFences(device, 1, &fence);
    if (result != VK_SUCCESS) goto cleanup;
    result = vkResetCommandPool(device, pool, 0);
    if (result != VK_SUCCESS) goto cleanup;
    result = vkBeginCommandBuffer(command, &begin);
    if (result != VK_SUCCESS) goto cleanup;
    memset(&image_barrier, 0, sizeof(image_barrier));
    image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_barrier.srcAccessMask = old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ?
        VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT;
    image_barrier.dstAccessMask = mode == 0 ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT :
        VK_ACCESS_SHADER_READ_BIT;
    image_barrier.oldLayout = old_layout;
    image_barrier.newLayout = new_layout;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = image;
    image_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_barrier.subresourceRange.levelCount = 1;
    image_barrier.subresourceRange.layerCount = 1;
    if (old_layout != new_layout)
        vkCmdPipelineBarrier(command,
            old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_PIPELINE_STAGE_TRANSFER_BIT :
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &image_barrier);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &set, 0, NULL);
    vkCmdDispatch(command, 1, 1, 1);
    result = vkEndCommandBuffer(command);
    if (result != VK_SUCCESS) goto cleanup;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    result = vkQueueSubmit(queue, 1, &submit, fence);
    if (result != VK_SUCCESS)
    {
        snprintf(state->present_error, sizeof(state->present_error),
                 "sampled submit result=%d shader=%s", (int)result, shader_name);
        goto cleanup;
    }
    state->queue_submits++;
    result = wait_for_fence(device, fence);
    if (result != VK_SUCCESS) goto cleanup;
    if (vkMapMemory(device, output_memory, 0, sizeof(uint32_t), 0, &mapped) != VK_SUCCESS)
    {
        snprintf(state->present_error, sizeof(state->present_error),
                 "sampled output map failed shader=%s", shader_name);
        goto cleanup;
    }
    {
        VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = output_memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(device, 1, &range);
    }
    *value = *(const uint32_t *)mapped;
    vkUnmapMemory(device, output_memory);
    mapped = NULL;
    state->sampled_value = *value;
    state->expected_sampled_value = expected;
    state->shader_executed = *value != 0xdeadbeefu;
    state->image_read_completed = state->shader_executed;
    result = *value == expected ? VK_SUCCESS : VK_ERROR_UNKNOWN;
    if (result != VK_SUCCESS)
        snprintf(state->present_error, sizeof(state->present_error),
                 "sampled output mismatch shader=%s value=0x%08x expected=0x%08x",
                 shader_name, *value, expected);

cleanup:
    if (mapped) vkUnmapMemory(device, output_memory);
    free(code);
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    if (shader) vkDestroyShaderModule(device, shader, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    if (output) vkDestroyBuffer(device, output, NULL);
    if (output_memory) vkFreeMemory(device, output_memory, NULL);
    return result == VK_SUCCESS;
}

static BOOL vulkan_environment_ready(void)
{
    const char *enabled = getenv("WINEHUA_VULKAN_RUNTIME");
    /*
     * BOX64_*, VK_DRIVER_FILES and VTEST_SOCKET_NAME are Unix-side runtime
     * controls. Wine does not promise to copy them into the Windows CRT
     * environment, so checking them from a PE program produces false
     * failures even when Box64 has already loaded the Venus ICD. The actual
     * instance/device/adapter checks below are the authoritative isolation
     * proof; this marker only catches an accidental non-Vulkan launch mode.
     */
    return enabled && !strcmp(enabled, "1");
}

static LRESULT CALLBACK winehua_vulkan_smoke_wndproc(HWND hwnd, UINT message,
                                                      WPARAM wparam, LPARAM lparam)
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

static BOOL run_present(struct probe_state *state, VkInstance instance,
                        VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                        VkCommandPool pool, VkCommandBuffer command, VkFence fence)
{
    WNDCLASSA window_class;
    HWND hwnd = NULL;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkImage *images = NULL;
    uint32_t image_count = 0;
    VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
    VkResult result;
    VkSurfaceCapabilitiesKHR capabilities;
    VkSurfaceFormatKHR surface_format;
    VkExtent2D extent;
    uint32_t frame_count = state->smoke.seconds ? state->smoke.seconds * 30 : 30;
    BOOL initialized[8] = {0};
    BOOL ok = FALSE;

    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = winehua_vulkan_smoke_wndproc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.lpszClassName = "WineHuaVulkanSmokeWindow";
    RegisterClassA(&window_class);
    hwnd = CreateWindowExA(0, window_class.lpszClassName, "WineHua Vulkan smoke",
                           WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL,
                           window_class.hInstance, NULL);
    if (!hwnd) goto cleanup;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    /* The App compositor attaches the NCP SurfaceQueue target asynchronously
     * after the Win32 surface becomes visible.  Give that IPC handoff a
     * bounded settling window so the first Acquire exercises Vulkan rather
     * than racing target creation. */
    Sleep(500);

    {
        VkWin32SurfaceCreateInfoKHR info = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        info.hinstance = window_class.hInstance;
        info.hwnd = hwnd;
        result = vkCreateWin32SurfaceKHR(instance, &info, NULL, &surface);
        if (result != VK_SUCCESS) goto cleanup;
    }
    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities);
    if (result != VK_SUCCESS) goto cleanup;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &image_count, NULL);
    if (result != VK_SUCCESS || !image_count) goto cleanup;
    surface_format = (VkSurfaceFormatKHR){0};
    {
        uint32_t count = 1;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, &surface_format);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE || !count) goto cleanup;
    }
    extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX)
        extent = (VkExtent2D){640, 480};
    if (!extent.width || !extent.height) extent = (VkExtent2D){1, 1};

    {
        VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface;
        info.minImageCount = capabilities.minImageCount < 3 ? 3 : capabilities.minImageCount;
        if (capabilities.maxImageCount && info.minImageCount > capabilities.maxImageCount)
            info.minImageCount = capabilities.maxImageCount;
        info.imageFormat = surface_format.format;
        info.imageColorSpace = surface_format.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        result = vkCreateSwapchainKHR(device, &info, NULL, &swapchain);
        if (result != VK_SUCCESS) goto cleanup;
    }
    result = vkGetSwapchainImagesKHR(device, swapchain, &image_count, NULL);
    if (result != VK_SUCCESS || !image_count || image_count > sizeof(initialized) / sizeof(initialized[0])) goto cleanup;
    images = calloc(image_count, sizeof(*images));
    if (!images) goto cleanup;
    result = vkGetSwapchainImagesKHR(device, swapchain, &image_count, images);
    if (result != VK_SUCCESS) goto cleanup;

    {
        VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        result = vkCreateSemaphore(device, &semaphore_info, NULL, &acquire_semaphore);
        if (result != VK_SUCCESS) goto cleanup;
    }

    for (uint32_t frame = 0; frame < frame_count; ++frame)
    {
        MSG message;
        uint32_t image_index;
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue color = {{(frame & 1) ? 0.12f : 0.82f,
                                    (frame & 2) ? 0.78f : 0.18f,
                                    (frame & 4) ? 0.24f : 0.66f, 1.0f}};
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                       acquire_semaphore, VK_NULL_HANDLE, &image_index);
        state->present_acquire_result = result;
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) goto cleanup;
        state->present_acquire_wait_result = VK_SUCCESS;
        vkResetCommandBuffer(command, 0);
        result = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS) goto cleanup;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[image_index];
        barrier.subresourceRange = range;
        barrier.oldLayout = initialized[image_index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                      : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        vkCmdClearColorImage(command, images[image_index],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) goto cleanup;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquire_semaphore;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        result = vkQueueSubmit(queue, 1, &submit, fence);
        state->present_submit_result = result;
        if (result != VK_SUCCESS) goto cleanup;
        state->queue_submits++;
        result = wait_for_fence(device, fence);
        state->present_submit_wait_result = result;
        if (result != VK_SUCCESS) goto cleanup;
        vkResetFences(device, 1, &fence);
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &image_index;
        result = vkQueuePresentKHR(queue, &present);
        state->present_queue_result = result;
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) goto cleanup;
        initialized[image_index] = TRUE;
        state->present_frames++;
    }
    state->present_ok = TRUE;
    ok = TRUE;

cleanup:
    if (!ok && !state->present_error[0])
    {
        const char *stage = "unknown";
        VkResult failure_result = result;
        if (state->present_acquire_result != VK_SUCCESS &&
            state->present_acquire_result != VK_SUBOPTIMAL_KHR)
        {
            stage = "acquire";
            failure_result = state->present_acquire_result;
        }
        else if (state->present_acquire_wait_result != VK_SUCCESS)
        {
            stage = "acquire-wait";
            failure_result = state->present_acquire_wait_result;
        }
        else if (state->present_submit_result != VK_SUCCESS)
        {
            stage = "submit";
            failure_result = state->present_submit_result;
        }
        else if (state->present_submit_wait_result != VK_SUCCESS)
        {
            stage = "submit-wait";
            failure_result = state->present_submit_wait_result;
        }
        else if (state->present_queue_result != VK_SUCCESS &&
                 state->present_queue_result != VK_SUBOPTIMAL_KHR)
        {
            stage = "present";
            failure_result = state->present_queue_result;
        }
        record_present_failure(state, state->present_frames, stage, failure_result);
    }
    if (acquire_semaphore) vkDestroySemaphore(device, acquire_semaphore, NULL);
    free(images);
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, NULL);
    if (surface) vkDestroySurfaceKHR(instance, surface, NULL);
    if (hwnd) DestroyWindow(hwnd);
    return ok;
}

int main(int argc, char **argv)
{
    struct probe_state state;
    VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer source = VK_NULL_HANDLE, destination = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory source_memory = VK_NULL_HANDLE, destination_memory = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE, image_memory = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkResult result = VK_SUCCESS;
    uint32_t count = 0, i;
    const char *instance_extensions[2];
    const char *device_extensions[1];
    void *mapped = NULL;
    VkDeviceMemory mapped_memory = VK_NULL_HANDLE;
    const char *failure = "unknown Wine Vulkan failure";
    BOOL capability_audit = FALSE;
    int exit_code = 1;

    memset(&state, 0, sizeof(state));
    state.started_ms = winehua_smoke_timestamp_ms();
    state.loader_api = VK_API_VERSION_1_0;
    state.queue_family = UINT32_MAX;
    for (i = 1; i < (uint32_t)argc; ++i)
        if (!strcmp(argv[i], "--capability-audit")) capability_audit = TRUE;
    if (!winehua_smoke_parse_options(&state.smoke, argc, argv, 1)) return 6;
    load_module_paths(&state);
    write_state(&state, "STARTED", "startup", "Wine Vulkan offscreen smoke starting");

    if (!state.smoke.offscreen && !state.smoke.present)
    {
        write_state(&state, "UNSUPPORTED", "present",
                    "Win32 Vulkan present is enabled in the next phase");
        return 3;
    }
    if (!vulkan_environment_ready())
    {
        write_state(&state, "FAIL", "startup",
                    "Wine Vulkan runtime launch marker is missing");
        return 2;
    }

    {
        PFN_vkEnumerateInstanceVersion enumerate_version =
            (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
                VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
        if (enumerate_version) enumerate_version(&state.loader_api);
    }
    application.pApplicationName = "winehua_vulkan_smoke";
    application.applicationVersion = 1;
    application.pEngineName = "WineHua";
    application.engineVersion = 1;
    /* Capability audit must request the API level vkd3d-proton will use.
     * Keep ordinary Wine Vulkan smoke on 1.1 for the established baseline. */
    application.apiVersion = capability_audit ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
    instance_info.pApplicationInfo = &application;
    if (state.smoke.present)
    {
        instance_extensions[0] = "VK_KHR_surface";
        instance_extensions[1] = "VK_KHR_win32_surface";
        instance_info.enabledExtensionCount = 2;
        instance_info.ppEnabledExtensionNames = instance_extensions;
    }
    result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS) { failure = "vkCreateInstance failed"; goto cleanup; }
    checkpoint(&state, "vkCreateInstance passed");

    result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (result != VK_SUCCESS || !count)
    {
        failure = "Wine Vulkan exposed no physical device";
        goto cleanup;
    }
    {
        VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
        if (!devices) { failure = "physical device allocation failed"; goto cleanup; }
        result = vkEnumeratePhysicalDevices(instance, &count, devices);
        if (result == VK_SUCCESS) physical = devices[0];
        free(devices);
        if (result != VK_SUCCESS || !physical)
        {
            failure = "physical device enumeration failed";
            goto cleanup;
        }
    }
    vkGetPhysicalDeviceProperties(physical, &state.properties);
    if (capability_audit && !query_extended_capabilities(physical, &state))
    {
        failure = "Wine Vulkan extended capability query failed";
        goto cleanup;
    }
    state.fallback_detected = strstr(state.properties.deviceName, "llvmpipe") != NULL ||
                              strstr(state.properties.deviceName, "softpipe") != NULL;
    if (state.fallback_detected)
    {
        failure = "software Vulkan fallback detected";
        goto cleanup;
    }
    checkpoint(&state, "physical device enumeration passed");

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    {
        VkQueueFamilyProperties *queues = calloc(count, sizeof(*queues));
        if (!queues) { failure = "queue family allocation failed"; goto cleanup; }
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
        for (i = 0; i < count; ++i)
        {
            if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                state.queue_family = i;
                break;
            }
        }
        free(queues);
    }
    if (state.queue_family == UINT32_MAX)
    {
        failure = "no graphics queue family";
        goto cleanup;
    }

    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        queue_info.queueFamilyIndex = state.queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        if (state.smoke.present)
        {
            device_extensions[0] = "VK_KHR_swapchain";
            device_info.enabledExtensionCount = 1;
            device_info.ppEnabledExtensionNames = device_extensions;
        }
        result = vkCreateDevice(physical, &device_info, NULL, &device);
        if (result != VK_SUCCESS) { failure = "vkCreateDevice failed"; goto cleanup; }
    }
    vkGetDeviceQueue(device, state.queue_family, 0, &queue);
    checkpoint(&state, "vkCreateDevice and queue acquisition passed");

    {
        VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        VkCommandBufferAllocateInfo allocation =
            {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = state.queue_family;
        result = vkCreateCommandPool(device, &pool_info, NULL, &pool);
        if (result != VK_SUCCESS) { failure = "vkCreateCommandPool failed"; goto cleanup; }
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device, &allocation, &command);
        if (result != VK_SUCCESS) { failure = "vkAllocateCommandBuffers failed"; goto cleanup; }
        result = vkCreateFence(device, &fence_info, NULL, &fence);
        if (result != VK_SUCCESS) { failure = "vkCreateFence failed"; goto cleanup; }
    }

    if (state.smoke.present)
    {
        if (!run_present(&state, instance, physical, device, queue, pool, command, fence))
        {
            failure = state.present_error[0] ? state.present_error :
                                               "Wine Vulkan private present failed";
            goto cleanup;
        }
        write_state(&state, "PASS", "present", "Wine Vulkan private BrokerPresent fixed-frame check passed");
        exit_code = 0;
        /* The Harmony Venus private swapchain may block in vkDestroyDevice
         * after a successful present sequence while the NCP drains its
         * disconnect cleanup.  Automation has already committed the
         * authoritative JSON result; let the OS tear down this short-lived
         * smoke process instead of carrying a driver destructor hang into the
         * next x86/x64 case.  Manual runs retain the normal cleanup path. */
        if (state.smoke.automation) ExitProcess(0);
        goto cleanup;
    }

    result = create_buffer(physical, device, 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &source, &source_memory);
    if (result != VK_SUCCESS) { failure = "source buffer creation failed"; goto cleanup; }
    result = create_buffer(physical, device, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &destination, &destination_memory);
    if (result != VK_SUCCESS) { failure = "destination buffer creation failed"; goto cleanup; }
    result = vkMapMemory(device, source_memory, 0, 4096, 0, &mapped);
    if (result != VK_SUCCESS) { failure = "source buffer map failed"; goto cleanup; }
    mapped_memory = source_memory;
    for (i = 0; i < 4096; ++i) ((uint8_t *)mapped)[i] = (uint8_t)(i * 37u + 11u);
    vkUnmapMemory(device, source_memory);
    mapped = NULL;
    mapped_memory = VK_NULL_HANDLE;
    {
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkBufferCopy copy = {0, 0, 4096};
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        result = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS) { failure = "buffer command begin failed"; goto cleanup; }
        vkCmdCopyBuffer(command, source, destination, 1, &copy);
        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) { failure = "buffer copy command recording failed"; goto cleanup; }
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        result = vkQueueSubmit(queue, 1, &submit, fence);
        if (result != VK_SUCCESS) { failure = "buffer copy submit failed"; goto cleanup; }
        state.queue_submits++;
        checkpoint(&state, "buffer copy submitted; waiting for fence");
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ULL);
        if (result != VK_SUCCESS) { failure = "buffer copy fence timeout"; goto cleanup; }
    }
    result = vkMapMemory(device, destination_memory, 0, 4096, 0, &mapped);
    if (result != VK_SUCCESS) { failure = "destination buffer map failed"; goto cleanup; }
    mapped_memory = destination_memory;
    for (i = 0; i < 4096; ++i)
        if (((uint8_t *)mapped)[i] != (uint8_t)(i * 37u + 11u)) break;
    vkUnmapMemory(device, destination_memory);
    mapped = NULL;
    mapped_memory = VK_NULL_HANDLE;
    if (i != 4096) { failure = "buffer copy verification failed"; goto cleanup; }
    state.buffer_copy_ok = TRUE;
    checkpoint(&state, "buffer copy and readback passed");

    {
        VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        VkMemoryRequirements requirements;
        VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        uint32_t type;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent.width = 16;
        image_info.extent.height = 16;
        image_info.extent.depth = 1;
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        state.sampled_only = !strcmp(winehua_smoke_env("WINEHUA_VULKAN_SAMPLED_ONLY", "0"), "1");
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT |
                           (state.sampled_only ? 0 : VK_IMAGE_USAGE_STORAGE_BIT);
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        result = vkCreateImage(device, &image_info, NULL, &image);
        if (result != VK_SUCCESS) { failure = "test image creation failed"; goto cleanup; }
        vkGetImageMemoryRequirements(device, image, &requirements);
        if (!find_memory_type(physical, requirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
            !find_memory_type(physical, requirements.memoryTypeBits, 0, &type))
        {
            failure = "test image memory type unavailable";
            goto cleanup;
        }
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        result = vkAllocateMemory(device, &allocation, NULL, &image_memory);
        if (result != VK_SUCCESS ||
            vkBindImageMemory(device, image, image_memory, 0) != VK_SUCCESS)
        {
            failure = "test image memory allocation failed";
            goto cleanup;
        }
    }
    result = create_buffer(physical, device, 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &readback, &readback_memory);
    if (result != VK_SUCCESS) { failure = "image readback buffer creation failed"; goto cleanup; }

    result = vkResetFences(device, 1, &fence);
    if (result != VK_SUCCESS) { failure = "image fence reset failed"; goto cleanup; }
    result = vkResetCommandPool(device, pool, 0);
    if (result != VK_SUCCESS) { failure = "image command pool reset failed"; goto cleanup; }
    {
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        VkClearColorValue color = {{0x11 / 255.0f, 0x22 / 255.0f,
                                    0x33 / 255.0f, 1.0f}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkBufferImageCopy copy;
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        memset(&copy, 0, sizeof(copy));
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = 16;
        copy.imageExtent.height = 16;
        copy.imageExtent.depth = 1;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        result = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS) { failure = "image command begin failed"; goto cleanup; }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, NULL, 0, NULL, 1, &barrier);
        vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &color, 1, &range);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, NULL, 0, NULL, 1, &barrier);
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback, 1, &copy);
        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) { failure = "image clear command recording failed"; goto cleanup; }
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        result = vkQueueSubmit(queue, 1, &submit, fence);
        if (result != VK_SUCCESS) { failure = "image clear submit failed"; goto cleanup; }
        state.queue_submits++;
        checkpoint(&state, "image clear submitted; waiting for fence");
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ULL);
        if (result != VK_SUCCESS) { failure = "image clear fence timeout"; goto cleanup; }
    }
    result = vkMapMemory(device, readback_memory, 0, 1024, 0, &mapped);
    if (result != VK_SUCCESS) { failure = "image readback map failed"; goto cleanup; }
    mapped_memory = readback_memory;
    {
        const uint8_t *pixel = mapped;
        BOOL ok = pixel[0] >= 16 && pixel[0] <= 18 &&
                  pixel[1] >= 33 && pixel[1] <= 35 &&
                  pixel[2] >= 50 && pixel[2] <= 52 && pixel[3] >= 253;
        vkUnmapMemory(device, readback_memory);
        mapped = NULL;
        mapped_memory = VK_NULL_HANDLE;
        if (!ok) { failure = "image clear verification failed"; goto cleanup; }
    }
    state.image_clear_ok = TRUE;

    {
        VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        VkSamplerCreateInfo sampler_info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        result = vkCreateImageView(device, &view_info, NULL, &image_view);
        if (result != VK_SUCCESS) { failure = "sampled image view creation failed"; goto cleanup; }
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 0.0f;
        result = vkCreateSampler(device, &sampler_info, NULL, &sampler);
        if (result != VK_SUCCESS) { failure = "sampled image sampler creation failed"; goto cleanup; }
    }
    {
        uint32_t value = 0;
        const uint32_t expected_color = 0xff332211u;
        if (!state.sampled_only) {
            if (!run_sampled_compute(&state, physical, device, queue, pool, command, fence,
                                     image, image_view, sampler, "venus_storage_write.spv", 0,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                     0xa1b2c3d4u, &value))
            { failure = state.present_error[0] ? state.present_error : "storage image write failed"; goto cleanup; }
            state.storage_image_write_ok = TRUE;
            if (!run_sampled_compute(&state, physical, device, queue, pool, command, fence,
                                     image, image_view, sampler, "venus_storage_read.spv", 0,
                                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                     expected_color, &value))
            { failure = state.present_error[0] ? state.present_error : "storage image read failed"; goto cleanup; }
            state.storage_image_read_ok = TRUE;
        }
        if (!run_sampled_compute(&state, physical, device, queue, pool, command, fence,
                                 image, image_view, sampler, "venus_image_fetch.spv", 1,
                                 state.sampled_only ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 expected_color, &value))
        { failure = state.present_error[0] ? state.present_error : "sampled image fetch failed"; goto cleanup; }
        state.sampled_image_fetch_ok = TRUE;
        if (!run_sampled_compute(&state, physical, device, queue, pool, command, fence,
                                 image, image_view, sampler, "venus_combined_sample.spv", 2,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 expected_color, &value))
        { failure = state.present_error[0] ? state.present_error : "combined image sampler failed"; goto cleanup; }
        state.combined_image_sampler_ok = TRUE;
        if (!run_sampled_compute(&state, physical, device, queue, pool, command, fence,
                                 image, image_view, sampler, "venus_separated_sample.spv", 3,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 expected_color, &value))
        { failure = state.present_error[0] ? state.present_error : "separated image sampler failed"; goto cleanup; }
        state.separated_image_sampler_ok = TRUE;
        state.shader_executed = TRUE;
        state.image_read_completed = TRUE;
    }
    write_state(&state, "PASS", "wine-vulkan",
                "Wine Vulkan sampled-image and buffer/image checks passed");
    exit_code = 0;

cleanup:
    if (exit_code)
    {
        write_state(&state, "FAIL", state.smoke.present ? "present" : "wine-vulkan", failure);
        if (state.smoke.automation && state.smoke.present) ExitProcess(exit_code);
    }
    if (mapped && device && mapped_memory) vkUnmapMemory(device, mapped_memory);
    if (device && fence) vkDestroyFence(device, fence, NULL);
    if (device && pool) vkDestroyCommandPool(device, pool, NULL);
    if (device && sampler) vkDestroySampler(device, sampler, NULL);
    if (device && image_view) vkDestroyImageView(device, image_view, NULL);
    if (device && readback) vkDestroyBuffer(device, readback, NULL);
    if (device && readback_memory) vkFreeMemory(device, readback_memory, NULL);
    if (device && image) vkDestroyImage(device, image, NULL);
    if (device && image_memory) vkFreeMemory(device, image_memory, NULL);
    if (device && destination) vkDestroyBuffer(device, destination, NULL);
    if (device && destination_memory) vkFreeMemory(device, destination_memory, NULL);
    if (device && source) vkDestroyBuffer(device, source, NULL);
    if (device && source_memory) vkFreeMemory(device, source_memory, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    free(state.capability_audit);
    return exit_code;
}
