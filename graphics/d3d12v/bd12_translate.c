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
__thread uint32_t g_bd12_compute_root_push_bytes;   /* read by bd12_compute_pipeline_build */

static BD12ComputePipeline *create_dxbc(BD12Device *device, const uint8_t *dxbc, size_t dxbc_size, uint64_t hash,
                                        const BD12RootSignature *rs);

BD12ComputePipeline *bd12_compute_pipeline_create_dxbc(
    BD12Device *device, const uint8_t *dxbc, size_t dxbc_size, uint64_t hash)
{
    return create_dxbc(device, dxbc, dxbc_size, hash, NULL);
}

/* BEER_BD12_CS_WATCH=<hash>[,<hash>...] (diagnostic): logs those compute PSOs' creation and counts their dispatches
 * (bd12_cs_watch_count, printed every 600 presents). Tells "never dispatched" apart from "PSO never created". */
static uint64_t g_cs_watch[16];
static uint64_t g_cs_watch_hits[16];
static int g_cs_watch_n = -1;
int bd12_cs_watch_index(uint64_t hash)
{
    if (g_cs_watch_n < 0) {
        int n = 0;
        const char *v = getenv("BEER_BD12_CS_WATCH");
        while (v && *v && n < 16) {
            char *end;
            g_cs_watch[n] = strtoull(v, &end, 16);
            if (end == v) break;
            ++n;
            v = *end == ',' ? end + 1 : end;
        }
        g_cs_watch_n = n;
    }
    for (int i = 0; i < g_cs_watch_n; ++i) if (g_cs_watch[i] == hash) return i;
    return -1;
}
void bd12_cs_watch_count(uint64_t hash)
{
    if (!g_cs_watch_n) return;
    const int i = bd12_cs_watch_index(hash);
    if (i >= 0) __atomic_add_fetch(&g_cs_watch_hits[i], 1, __ATOMIC_RELAXED);
}
void bd12_cs_watch_report(void)
{
    static unsigned presents;
    if (g_cs_watch_n <= 0 || ++presents % 600) return;
    for (int i = 0; i < g_cs_watch_n; ++i)
        fprintf(stderr, "[CSWATCH] %016llx dispatches %llu (after %u presents)\n", (unsigned long long)g_cs_watch[i],
                (unsigned long long)__atomic_load_n(&g_cs_watch_hits[i], __ATOMIC_RELAXED), presents);
}
BD12ComputePipeline *bd12_compute_pipeline_create_dxbc_rs(
    BD12Device *device, const uint8_t *dxbc, size_t dxbc_size, uint64_t hash, BD12RootSignature *root_signature)
{
    BD12ComputePipeline *result = NULL;
    if (root_signature && bd12_root_push_enabled(device) && bd12_root_layout(root_signature)) {
        BD12ComputePipeline *pipeline = create_dxbc(device, dxbc, dxbc_size, hash, root_signature);
        bd12_root_push_count(0, pipeline != NULL);
        result = pipeline;   /* otherwise: per-dispatch descriptors */
    }
    if (!result) result = create_dxbc(device, dxbc, dxbc_size, hash, NULL);
    if (bd12_cs_watch_index(hash) >= 0)
        fprintf(stderr, "[CSWATCH] compute PSO %016llx: %s\n", (unsigned long long)hash,
                result ? (result->db_layout ? "created (root push)" : "created (per-dispatch descriptors)") : "FAILED");
    return result;
}

static BD12ComputePipeline *create_dxbc(BD12Device *device, const uint8_t *dxbc, size_t dxbc_size, uint64_t hash,
                                        const BD12RootSignature *rs)
{
    const BeerD3D12TranslatedShader *shader = rs ?
        beer_d3d12_translate_shader_rs(dxbc, dxbc_size, hash, rs->root_layout) :
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
            .heap_class = d->heap_class,
            .is_ssbo = d->ssbo
        };
    }
    BD12ComputePipeline *pipeline;
    if (rs && rs->root_layout->heap_model == BEER_HEAP_MODEL_MUTABLE) {
        const BD12DbLayout *db_layout = bd12_db_layout(rs);
        pipeline = db_layout ? bd12_compute_pipeline_build_db(device, (const uint32_t *)shader->spirv,
                                                              shader->spirv_size, db_layout, rs->root_push_bytes,
                                                              rs->root_layout->hash)
                             : NULL;
    } else {
        g_bd12_compute_root_push_bytes = rs ? rs->root_push_bytes : 0;
        pipeline = bd12_compute_pipeline_create_bound(device, (const uint32_t *)shader->spirv, shader->spirv_size,
                                                      bindings, shader->descriptor_count);
        g_bd12_compute_root_push_bytes = 0;
        if (pipeline && rs) pipeline->root_layout_hash = rs->root_layout->hash;
    }
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
