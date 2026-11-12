#ifndef BD12_CUDA_H
#define BD12_CUDA_H
/* CUDA-kernel interop on the BD12 device, the D3D12 side of NVAPI's
 * NvAPI_D3D12_*Cubin* / GetCuda*Object functions (NVIDIA NGX runs DLSS through them). Built on
 * VK_NVX_binary_import (CUDA modules, functions, kernel launches) and VK_NVX_image_view_handle (32-bit view
 * handles usable as cudaTextureObject_t / cudaSurfaceObject_t). Buffer GPU virtual addresses are Vulkan device
 * addresses, which CUDA kernels dereference directly on NVIDIA hardware.
 *
 * Results are NVAPI status codes: 0 NVAPI_OK, -5 NVAPI_INVALID_ARGUMENT, -104 NVAPI_NOT_SUPPORTED, -1 NVAPI_ERROR. */
#include <stdint.h>

#define BD12_NVAPI_OK 0
#define BD12_NVAPI_ERROR (-1)
#define BD12_NVAPI_INVALID_ARGUMENT (-5)
#define BD12_NVAPI_NOT_SUPPORTED (-104)

/* Nonzero when the device was created with both NVX extensions and their entry points resolved. */
int bd12_cuda_available(void);

/* `device` is the guest ID3D12Device. dynamic_shared is CUDA dynamic shared memory per launch; name selects the
 * kernel in the module (NULL or "" uses the module's only/first entry, as the unnamed NVAPI variant does). */
int bd12_cuda_texturing64(void);
int bd12_cuda_capture_uav_info(void *device, void *info);
struct BD12Device;
void bd12_cuda_uav_created(struct BD12Device *device, const void *descriptor);
int bd12_cuda_create_shader(void *device, const void *cubin, uint32_t size, uint32_t block_x, uint32_t block_y,
                            uint32_t block_z, uint32_t dynamic_shared, const char *name, int texturing64,
                            uint64_t *shader);
int bd12_cuda_view_handle64(void *device, uint64_t view_descriptor, uint64_t sampler_descriptor, int surface,
                            uint64_t *handle);
int bd12_cuda_destroy_shader(void *device, uint64_t shader);
/* Records the launch into the open ID3D12GraphicsCommandList. params is the kernel parameter buffer. */
int bd12_cuda_launch(void *command_list, uint64_t shader, uint32_t grid_x, uint32_t grid_y, uint32_t grid_z,
                     const void *params, uint32_t params_size);
/* D3D12 CPU descriptor handles (SRV + sampler, or UAV) -> 32-bit CUDA texture / surface object. */
int bd12_cuda_texture_object(void *device, uint64_t srv_descriptor, uint64_t sampler_descriptor, uint32_t *handle);
int bd12_cuda_surface_object(void *device, uint64_t uav_descriptor, uint32_t *handle);

#endif
