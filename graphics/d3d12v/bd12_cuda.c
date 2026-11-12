#define _GNU_SOURCE
#include "bd12_cuda.h"
#include "bd12_com.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

BD12CmdList *com_list_open_commands(ComObj *list);   /* bd12_com_list.c */
void bd12_cmd_end_rendering(BD12CmdList *list);       /* bd12_graphics.c */

static struct {
    int state;                    /* 0 unknown, 1 ready, -1 unavailable */
    PFN_vkCreateCuModuleNVX create_module;
    PFN_vkDestroyCuModuleNVX destroy_module;
    PFN_vkCreateCuFunctionNVX create_function;
    PFN_vkDestroyCuFunctionNVX destroy_function;
    PFN_vkCmdCuLaunchKernelNVX launch;
    PFN_vkGetImageViewHandleNVX view_handle;
    PFN_vkGetImageViewHandle64NVX view_handle64;
    PFN_vkGetImageViewAddressNVX view_address;
    int texturing64;              /* 64-bit CUDA texturing: VK_NVX_binary_import >= 2 and VK_NVX_image_view_handle >= 3 */
} g_cuda;

#define CUDA_SHADER_MAGIC 0x43554442u   /* "BDUC" */
typedef struct {
    uint32_t magic;
    VkCuModuleNVX module;
    VkCuFunctionNVX function;
    uint32_t block[3];
    uint32_t dynamic_shared;
    char name[64];
} CudaShader;

static BD12Device *device_of(void *device)
{
    ComDevice *dev = device;
    return dev && dev == g_com_device ? dev->bd : NULL;
}

static int ensure_loaded(BD12Device *bd)
{
    if (g_cuda.state) return g_cuda.state > 0;
    g_cuda.state = -1;
    if (!bd || !beer_vulkan_nvx_cuda_enabled()) return 0;
    PFN_vkGetDeviceProcAddr get = beer_vulkan_get_device_proc_addr();
    if (!get) return 0;
#define LOAD(field, name) g_cuda.field = (void *)get(bd->device, name)
    LOAD(create_module, "vkCreateCuModuleNVX");
    LOAD(destroy_module, "vkDestroyCuModuleNVX");
    LOAD(create_function, "vkCreateCuFunctionNVX");
    LOAD(destroy_function, "vkDestroyCuFunctionNVX");
    LOAD(launch, "vkCmdCuLaunchKernelNVX");
    LOAD(view_handle, "vkGetImageViewHandleNVX");
    LOAD(view_handle64, "vkGetImageViewHandle64NVX");
    LOAD(view_address, "vkGetImageViewAddressNVX");
#undef LOAD
    {   /* 64-bit texturing (CreateCubinComputeShaderExV2, merged / independent descriptor objects) needs module
         * texturing-mode control (binary_import rev 2) and 64-bit view handles (image_view_handle rev 3), as
         * vkd3d-proton checks. */
        PFN_vkGetInstanceProcAddr gipa = beer_vulkan_get_instance_proc_addr();
        PFN_vkEnumerateDeviceExtensionProperties enumerate = gipa ?
            (PFN_vkEnumerateDeviceExtensionProperties)gipa(bd->instance, "vkEnumerateDeviceExtensionProperties") : NULL;
        uint32_t count = 0, binary_rev = 0, handle_rev = 0;
        if (enumerate && enumerate(bd->physical_device, NULL, &count, NULL) == VK_SUCCESS && count) {
            VkExtensionProperties *ext = calloc(count, sizeof(*ext));
            if (ext && enumerate(bd->physical_device, NULL, &count, ext) == VK_SUCCESS)
                for (uint32_t i = 0; i < count; ++i) {
                    if (!strcmp(ext[i].extensionName, VK_NVX_BINARY_IMPORT_EXTENSION_NAME)) binary_rev = ext[i].specVersion;
                    if (!strcmp(ext[i].extensionName, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME)) handle_rev = ext[i].specVersion;
                }
            free(ext);
        }
        g_cuda.texturing64 = g_cuda.view_handle64 && binary_rev >= 2 && handle_rev >= 3;
    }
    if (g_cuda.create_module && g_cuda.destroy_module && g_cuda.create_function && g_cuda.destroy_function &&
        g_cuda.launch && g_cuda.view_handle)
        g_cuda.state = 1;
    fprintf(stderr, "[NVAPI] CUDA interop (VK_NVX_binary_import/image_view_handle): %s, 64-bit texturing %s\n",
            g_cuda.state > 0 ? "ready" : "entry points missing", g_cuda.texturing64 ? "yes" : "no");
    return g_cuda.state > 0;
}

int bd12_cuda_available(void)
{
    return g_com_device && ensure_loaded(g_com_device->bd);
}

int bd12_cuda_texturing64(void)
{
    return g_com_device && ensure_loaded(g_com_device->bd) && g_cuda.texturing64;
}

int bd12_cuda_create_shader(void *device, const void *cubin, uint32_t size, uint32_t block_x, uint32_t block_y,
                            uint32_t block_z, uint32_t dynamic_shared, const char *name, int texturing64,
                            uint64_t *shader)
{
    if (shader) *shader = 0;
    BD12Device *bd = device_of(device);
    if (!bd || !cubin || !size || !shader) return BD12_NVAPI_INVALID_ARGUMENT;
    if (!ensure_loaded(bd)) return BD12_NVAPI_NOT_SUPPORTED;
    if (texturing64 && !g_cuda.texturing64) return BD12_NVAPI_NOT_SUPPORTED;
    CudaShader *s = calloc(1, sizeof(*s));
    if (!s) return BD12_NVAPI_ERROR;
    /* ExV2 modules take 64-bit texture/surface handles (merged and independent descriptor objects). */
    const VkCuModuleTexturingModeCreateInfoNVX texturing = {
        .sType = VK_STRUCTURE_TYPE_CU_MODULE_TEXTURING_MODE_CREATE_INFO_NVX, .use64bitTexturing = VK_TRUE,
    };
    const VkCuModuleCreateInfoNVX module_info = {
        .sType = VK_STRUCTURE_TYPE_CU_MODULE_CREATE_INFO_NVX, .pNext = texturing64 ? &texturing : NULL,
        .dataSize = size, .pData = cubin,
    };
    VkResult result = g_cuda.create_module(bd->device, &module_info, NULL, &s->module);
    if (result == VK_SUCCESS) {
        const VkCuFunctionCreateInfoNVX function_info = {
            .sType = VK_STRUCTURE_TYPE_CU_FUNCTION_CREATE_INFO_NVX, .module = s->module, .pName = name ? name : "",
        };
        result = g_cuda.create_function(bd->device, &function_info, NULL, &s->function);
        if (result != VK_SUCCESS) g_cuda.destroy_module(bd->device, s->module, NULL);
    }
    fprintf(stderr, "[NVAPI] CreateCubinComputeShader(%u bytes, block %ux%ux%u, shared %u, \"%s\"%s) -> %d\n", size,
            block_x, block_y, block_z, dynamic_shared, name ? name : "", texturing64 ? ", 64-bit texturing" : "",
            (int)result);
    if (result != VK_SUCCESS) { free(s); return BD12_NVAPI_ERROR; }
    s->magic = CUDA_SHADER_MAGIC;
    s->block[0] = block_x; s->block[1] = block_y; s->block[2] = block_z;
    s->dynamic_shared = dynamic_shared;
    snprintf(s->name, sizeof(s->name), "%s", name ? name : "");
    *shader = (uint64_t)(uintptr_t)s;
    return BD12_NVAPI_OK;
}

static CudaShader *shader_of(uint64_t handle)
{
    CudaShader *s = (CudaShader *)(uintptr_t)handle;
    return s && s->magic == CUDA_SHADER_MAGIC ? s : NULL;
}

/* Freed once GPU work recorded so far has retired: command lists may still reference the function. */
static void shader_free(ComDevice *dev, void *ptr)
{
    CudaShader *s = ptr;
    g_cuda.destroy_function(dev->bd->device, s->function, NULL);
    g_cuda.destroy_module(dev->bd->device, s->module, NULL);
    free(s);
}

int bd12_cuda_destroy_shader(void *device, uint64_t shader)
{
    BD12Device *bd = device_of(device);
    CudaShader *s = shader_of(shader);
    if (!bd || !s) return BD12_NVAPI_INVALID_ARGUMENT;
    s->magic = 0;
    com_defer(g_com_device, shader_free, s);
    return BD12_NVAPI_OK;
}

static void full_barrier(BD12CmdList *list)
{
    const VkMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier,
    };
    list->device->vk.CmdPipelineBarrier2(list->command_buffer, &dependency);
}

int bd12_cuda_launch(void *command_list, uint64_t shader, uint32_t grid_x, uint32_t grid_y, uint32_t grid_z,
                     const void *params, uint32_t params_size)
{
    CudaShader *s = shader_of(shader);
    BD12CmdList *list = command_list ? com_list_open_commands(command_list) : NULL;
    if (!s || !list) return BD12_NVAPI_INVALID_ARGUMENT;
    if (!ensure_loaded(list->device)) return BD12_NVAPI_NOT_SUPPORTED;
    bd12_cmd_end_rendering(list);
    /* The D3D12 barriers the caller recorded target compute/graphics stages; a kernel launch is ordered against
     * everything around it. */
    full_barrier(list);
    size_t size = params_size;
    const void *extras[] = {
        (const void *)(uintptr_t)1, params,          /* CU_LAUNCH_PARAM_BUFFER_POINTER */
        (const void *)(uintptr_t)2, &size,           /* CU_LAUNCH_PARAM_BUFFER_SIZE */
        (const void *)(uintptr_t)0,                  /* CU_LAUNCH_PARAM_END */
    };
    const VkCuLaunchInfoNVX launch = {
        .sType = VK_STRUCTURE_TYPE_CU_LAUNCH_INFO_NVX, .function = s->function,
        .gridDimX = grid_x, .gridDimY = grid_y, .gridDimZ = grid_z,
        .blockDimX = s->block[0], .blockDimY = s->block[1], .blockDimZ = s->block[2],
        .sharedMemBytes = s->dynamic_shared,
        .extraCount = params && params_size ? sizeof(extras) / sizeof(*extras) : 0,
        .pExtras = params && params_size ? extras : NULL,
    };
    g_cuda.launch(list->command_buffer, &launch);
    full_barrier(list);
    static int logged;
    if (logged++ < 8)
        fprintf(stderr, "[NVAPI] LaunchCubinShader(\"%s\", grid %ux%ux%u, %u param bytes)\n", s->name, grid_x, grid_y,
                grid_z, params_size);
    return BD12_NVAPI_OK;
}

int bd12_cuda_texture_object(void *device, uint64_t srv_descriptor, uint64_t sampler_descriptor, uint32_t *handle)
{
    if (handle) *handle = 0;
    BD12Device *bd = device_of(device);
    const BD12Descriptor *srv = (const BD12Descriptor *)(uintptr_t)srv_descriptor;
    const BD12Descriptor *sampler = (const BD12Descriptor *)(uintptr_t)sampler_descriptor;
    if (!bd || !srv || !sampler || !handle) return BD12_NVAPI_INVALID_ARGUMENT;
    if (!ensure_loaded(bd)) return BD12_NVAPI_NOT_SUPPORTED;
    if (srv->kind != BD12_DESC_SRV_IMAGE || !srv->image_view || sampler->kind != BD12_DESC_SAMPLER || !sampler->sampler)
        return BD12_NVAPI_INVALID_ARGUMENT;
    const VkImageViewHandleInfoNVX info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX, .imageView = srv->image_view,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .sampler = sampler->sampler,
    };
    *handle = g_cuda.view_handle(bd->device, &info);
    return BD12_NVAPI_OK;
}

int bd12_cuda_surface_object(void *device, uint64_t uav_descriptor, uint32_t *handle)
{
    if (handle) *handle = 0;
    BD12Device *bd = device_of(device);
    const BD12Descriptor *uav = (const BD12Descriptor *)(uintptr_t)uav_descriptor;
    if (!bd || !uav || !handle) return BD12_NVAPI_INVALID_ARGUMENT;
    if (!ensure_loaded(bd)) return BD12_NVAPI_NOT_SUPPORTED;
    if (uav->kind != BD12_DESC_UAV_IMAGE || !uav->image_view) return BD12_NVAPI_INVALID_ARGUMENT;
    const VkImageViewHandleInfoNVX info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX, .imageView = uav->image_view,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    };
    *handle = g_cuda.view_handle(bd->device, &info);
    return BD12_NVAPI_OK;
}

/* 64-bit handles (vkGetImageViewHandle64NVX) for 64-bit-texturing modules: a texture with its sampler (merged
 * object), a texture alone, or a surface (independent descriptor objects). */
int bd12_cuda_view_handle64(void *device, uint64_t view_descriptor, uint64_t sampler_descriptor, int surface,
                            uint64_t *handle)
{
    if (handle) *handle = 0;
    BD12Device *bd = device_of(device);
    const BD12Descriptor *view = (const BD12Descriptor *)(uintptr_t)view_descriptor;
    const BD12Descriptor *sampler = (const BD12Descriptor *)(uintptr_t)sampler_descriptor;
    if (!bd || !view || !handle) return BD12_NVAPI_INVALID_ARGUMENT;
    if (!ensure_loaded(bd) || !g_cuda.texturing64) return BD12_NVAPI_NOT_SUPPORTED;
    if (!view->image_view || (surface ? view->kind != BD12_DESC_UAV_IMAGE : view->kind != BD12_DESC_SRV_IMAGE))
        return BD12_NVAPI_INVALID_ARGUMENT;
    if (sampler && (sampler->kind != BD12_DESC_SAMPLER || !sampler->sampler)) return BD12_NVAPI_INVALID_ARGUMENT;
    const VkImageViewHandleInfoNVX info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX, .imageView = view->image_view,
        .descriptorType = surface ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                          sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .sampler = sampler ? sampler->sampler : VK_NULL_HANDLE,
    };
    *handle = g_cuda.view_handle64(bd->device, &info);
    return *handle ? BD12_NVAPI_OK : BD12_NVAPI_ERROR;
}

/* NvAPI_D3D12_CaptureUAVInfo: the caller's NVAPI_UAV_INFO (version, surfaceHandle, gpuVAStart, gpuVASize[, outFlags])
 * is filled by the next CreateUnorderedAccessView on the same thread (the driver's contract, as vkd3d-proton keeps it):
 * a buffer view's GPU address and range, or an image view's 32-bit surface handle with its address and size. */
static __thread uint8_t *t_uav_info;

int bd12_cuda_capture_uav_info(void *device, void *info)
{
    BD12Device *bd = device_of(device);
    if (!bd || !info) return BD12_NVAPI_INVALID_ARGUMENT;
    if (!ensure_loaded(bd)) return BD12_NVAPI_NOT_SUPPORTED;
    t_uav_info = info;
    return BD12_NVAPI_OK;
}

void bd12_cuda_uav_created(BD12Device *bd, const void *descriptor)
{
    uint8_t *info = t_uav_info;
    if (!info) return;
    t_uav_info = NULL;
    const BD12Descriptor *d = descriptor;
    uint32_t handle = 0;
    uint64_t va = 0, size = 0;
    if (d && d->kind == BD12_DESC_UAV_BUFFER) {
        va = d->source_buffer ? d->source_buffer->va + d->offset : 0;
        size = d->range;
    } else if (d && d->kind == BD12_DESC_UAV_IMAGE && d->image_view && bd) {
        const VkImageViewHandleInfoNVX hi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX,
                                              .imageView = d->image_view,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE };
        handle = g_cuda.view_handle(bd->device, &hi);
        VkImageViewAddressPropertiesNVX address = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_ADDRESS_PROPERTIES_NVX };
        if (g_cuda.view_address && g_cuda.view_address(bd->device, d->image_view, &address) == VK_SUCCESS) {
            va = address.deviceAddress;
            size = address.size;
        }
    }
    memcpy(info + 4, &handle, 4);
    memcpy(info + 8, &va, 8);
    memcpy(info + 16, &size, 8);
}
