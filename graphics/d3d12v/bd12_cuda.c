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
#undef LOAD
    if (g_cuda.create_module && g_cuda.destroy_module && g_cuda.create_function && g_cuda.destroy_function &&
        g_cuda.launch && g_cuda.view_handle)
        g_cuda.state = 1;
    fprintf(stderr, "[NVAPI] CUDA interop (VK_NVX_binary_import/image_view_handle): %s\n",
            g_cuda.state > 0 ? "ready" : "entry points missing");
    return g_cuda.state > 0;
}

int bd12_cuda_available(void)
{
    return g_com_device && ensure_loaded(g_com_device->bd);
}

int bd12_cuda_create_shader(void *device, const void *cubin, uint32_t size, uint32_t block_x, uint32_t block_y,
                            uint32_t block_z, uint32_t dynamic_shared, const char *name, uint64_t *shader)
{
    if (shader) *shader = 0;
    BD12Device *bd = device_of(device);
    if (!bd || !cubin || !size || !shader) return BD12_NVAPI_INVALID_ARGUMENT;
    if (!ensure_loaded(bd)) return BD12_NVAPI_NOT_SUPPORTED;
    CudaShader *s = calloc(1, sizeof(*s));
    if (!s) return BD12_NVAPI_ERROR;
    const VkCuModuleCreateInfoNVX module_info = {
        .sType = VK_STRUCTURE_TYPE_CU_MODULE_CREATE_INFO_NVX, .dataSize = size, .pData = cubin,
    };
    VkResult result = g_cuda.create_module(bd->device, &module_info, NULL, &s->module);
    if (result == VK_SUCCESS) {
        const VkCuFunctionCreateInfoNVX function_info = {
            .sType = VK_STRUCTURE_TYPE_CU_FUNCTION_CREATE_INFO_NVX, .module = s->module, .pName = name ? name : "",
        };
        result = g_cuda.create_function(bd->device, &function_info, NULL, &s->function);
        if (result != VK_SUCCESS) g_cuda.destroy_module(bd->device, s->module, NULL);
    }
    fprintf(stderr, "[NVAPI] CreateCubinComputeShader(%u bytes, block %ux%ux%u, shared %u, \"%s\") -> %d\n", size,
            block_x, block_y, block_z, dynamic_shared, name ? name : "", (int)result);
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
