#ifndef BEER_D3D12_VULKAN_COMPUTE_H
#define BEER_D3D12_VULKAN_COMPUTE_H

#include "d3d12_compat.h"
#include "d3d12_shader_translator.h"

/* Executes an arbitrary translated D3D12 compute dispatch on Vulkan.
 *
 * Descriptors the shader declares are matched against the snapshot's
 * root-signature-aware bindings (descriptor tables, root CBV/SRV/UAV, and
 * root constants) by register type, register index, and space. Only shaders
 * whose resources are all buffers are supported; any unresolved or
 * unsupported resource rejects the whole dispatch. Returns nonzero when the
 * dispatch executed. `*reason` receives a static explanation otherwise. */
int beer_d3d12_execute_vulkan_compute(
    const BeerD3D12ComputeDispatch *dispatch,
    const BeerD3D12TranslatedShader *shader, uint32_t hazards_before,
    const char **reason);

/* Consumes the backend-neutral compute IR as one ordered backend operation.
 * Translation, descriptor mapping, diagnostics, and result ownership live at
 * this boundary so the loader does not need to understand Vulkan execution.
 * The current implementation remains conservative internally; the renderer
 * can later compile the same batch into one Vulkan command buffer without
 * changing the D3D12 frontend or loader. */
void beer_d3d12_execute_vulkan_compute_batch(
    const BeerD3D12ComputeCommandBatch *batch, uint8_t *results);

#endif
