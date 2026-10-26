#define _GNU_SOURCE
#include "bd12.h"
#include "d3d12_shader_translator.h"

#include <stdio.h>
#include <stdlib.h>

/* SPIR-V opcodes used by the bounded dispatch flight recorder. */
#define BD12_SPV_OP_CONTROL_BARRIER 224u
#define BD12_SPV_OP_LOOP_MERGE 246u

/* Bridge: DXBC compute shader -> translated SPIR-V + reflection ->
 * BD12 compute pipeline with a record-time-resolved set-0 layout. */
BD12ComputePipeline *bd12_compute_pipeline_create_dxbc(
    BD12Device *device, const uint8_t *dxbc, size_t dxbc_size, uint64_t hash)
{
    const BeerD3D12TranslatedShader *shader =
        beer_d3d12_translate_shader(dxbc, dxbc_size, hash);
    if (!shader) return NULL;
    BD12ShaderBinding bindings[BEER_D3D12_MAX_SHADER_DESCRIPTORS];
    for (uint32_t i = 0; i < shader->descriptor_count; ++i) {
        const BeerD3D12ShaderDescriptor *d = &shader->descriptors[i];
        bindings[i] = (BD12ShaderBinding){
            .type = d->type,              /* enum values match BD12_RANGE_* */
            .space = d->register_space,
            .reg = d->register_index,
            .is_buffer = d->resource_type == BEER_D3D12_SHADER_RESOURCE_BUFFER,
            .is_accel = d->resource_type == BEER_D3D12_SHADER_RESOURCE_ACCEL,
            .count = d->count ? d->count : 1,
            .binding = d->binding,
            .bindless = d->bindless,
            .push_word = d->push_word,
            .heap_class = d->heap_class
        };
    }
    BD12ComputePipeline *pipeline = bd12_compute_pipeline_create_bound(
        device, (const uint32_t *)shader->spirv, shader->spirv_size, bindings,
        shader->descriptor_count);
    if (!pipeline) return NULL;
    pipeline->shader_hash = shader->shader_hash;
    const char *dump_dir = getenv("BEER_BD12_DUMP_SPIRV");
    if (dump_dir && *dump_dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/bd12-%016llx.spv", dump_dir,
                 (unsigned long long)shader->shader_hash);
        FILE *out = fopen(path, "wb");
        if (out) {
            fwrite(shader->spirv, 1, shader->spirv_size, out);
            fclose(out);
        }
    }
    for (uint32_t i = 0; i < shader->descriptor_count; ++i) {
        const BeerD3D12ShaderDescriptor *d = &shader->descriptors[i];
        if (d->type == BEER_D3D12_SHADER_DESCRIPTOR_UAV &&
            d->resource_type == BEER_D3D12_SHADER_RESOURCE_TEXTURE_3D)
            pipeline->writes_3d_storage_image = 1;
    }
    const uint32_t *words = (const uint32_t *)shader->spirv;
    const size_t word_count = shader->spirv_size / sizeof(*words);
    for (size_t at = 5; at < word_count; ) {
        const uint32_t instruction = words[at];
        const uint32_t length = instruction >> 16;
        const uint32_t opcode = instruction & 0xffffu;
        if (!length || length > word_count - at) break;
        pipeline->loop_count += opcode == BD12_SPV_OP_LOOP_MERGE;
        pipeline->control_barrier_count +=
            opcode == BD12_SPV_OP_CONTROL_BARRIER;
        at += length;
    }
    return pipeline;
}
