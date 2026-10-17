#ifndef BEER_D3D12_VULKAN_GRAPHICS_H
#define BEER_D3D12_VULKAN_GRAPHICS_H

#include "d3d12_compat.h"
#include "d3d12_shader_translator.h"

/* Executes translated D3D12 graphics through the generic Vulkan backend.
 * pixel may be NULL only for a depth-only draw with no color targets.
 * Unsupported API state is rejected explicitly. */
int beer_d3d12_execute_vulkan_graphics(
    const BeerD3D12GraphicsDraw *draw,
    const BeerD3D12TranslatedShader *vertex,
    const BeerD3D12TranslatedShader *pixel,
    const char **reason);

/* Executes several draw arguments that share one immutable D3D12 state
 * snapshot. The Vulkan backend records them in one render pass and performs
 * one queue submission/fence wait. */
uint32_t beer_d3d12_execute_vulkan_graphics_prepared_batch(
    const BeerD3D12GraphicsDraw *draw,
    const BeerD3D12TranslatedShader *vertex,
    const BeerD3D12TranslatedShader *pixel,
    const BeerD3D12GraphicsArguments *arguments, uint32_t argument_count,
    const char **reason);

/* Direct executor used by the command-queue replay fast path after shaders
 * have already been translated. Keeping diagnostics outside this call avoids
 * repeating loader-side tracing work for every generated indirect draw. */
int beer_d3d12_execute_vulkan_graphics_draw(
    const BeerD3D12GraphicsDraw *draw, const char **reason);

/* Executor-signature adapter for direct queue replay. */
int beer_d3d12_execute_vulkan_graphics_fast(
    const BeerD3D12GraphicsDraw *draw);

/* Batch adapter for compatible ExecuteIndirect expansions. Shared immutable
 * state is prepared once and all generated draw arguments are recorded into one
 * Vulkan render pass, command buffer, queue submission, and fence wait. */
uint32_t beer_d3d12_execute_vulkan_graphics_batch(
    const BeerD3D12GraphicsDraw *draw,
    const BeerD3D12GraphicsArguments *arguments, uint32_t argument_count);

#endif
