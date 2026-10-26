#ifndef BEER_D3D12_DXIL_TRANSLATOR_H
#define BEER_D3D12_DXIL_TRANSLATOR_H

#include "d3d12_shader_translator.h"

/* Nonzero when a DXBC container carries a DXIL (shader model 6) program. */
int beer_d3d12_is_dxil(const uint8_t *dxbc, size_t size);

/* Translates a DXIL container with dxil-spirv. `stage_namespace` is the binding
 * namespace (0 compute, 1 vertex, 2 pixel, ...) used to number descriptors.
 * Fills out->spirv (malloc'd), spirv_size and the descriptor list. */
int beer_d3d12_dxil_translate(const uint8_t *container, size_t size,
                              uint32_t stage_namespace,
                              BeerD3D12TranslatedShader *out);

/* One translated ray tracing entry point (raygen, miss, hit, intersection, callable). */
typedef struct BeerD3D12RtShader {
    char name[128];              /* demangled export name, e.g. "RG_Main" */
    uint32_t stage;              /* dxil_spv_shader_stage value (8 closest hit, 9 miss, 10 any hit, 11 raygen, ...) */
    uint8_t *spirv;              /* malloc'd */
    size_t spirv_size;
} BeerD3D12RtShader;

/* Translates the entry points of a DXIL library. Descriptors are collected into `shared` (one binding space for the
 * whole pipeline: the same register always gets the same binding, bindless arrays the same push word), so every shader
 * of a state object can use one pipeline layout. `max` entries of `out` are filled; *count receives how many. */
int beer_d3d12_dxil_translate_library(const uint8_t *container, size_t size, BeerD3D12TranslatedShader *shared,
                                      BeerD3D12RtShader *out, uint32_t max, uint32_t *count);

#endif
