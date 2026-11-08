#define _GNU_SOURCE
#include "bd12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Root arguments as push constants (BEER_BD12_ROOT_PUSH=1).
 *
 * The per-draw path builds a set-0 descriptor set from the bound root signature for every draw; engines change root
 * descriptor addresses and table handles on nearly every draw, so that set is rebuilt each time and dominates the
 * render thread. Here, as vkd3d-proton does, DXIL shaders are translated for their root signature instead: the push
 * block carries the root arguments themselves (root CBV/SRV/UAV GPU addresses, each table's first heap index, root
 * constants) and tables are read straight from the heap: with descriptor buffers (bd12_heapdb.c) the heaps are
 * sets 0/1 and static samplers set 2 of a device-owned layout; otherwise the bindless heap mirror is set 1 and
 * static samplers stay in a per-draw set 0. A draw then only pushes the arguments that changed. D3D12 limits a root signature to 64 DWORDs, which is
 * the layout below at most (a root descriptor costs two words), plus one word kept zero for direct heap access. */

static pthread_mutex_t g_layout_lock = PTHREAD_MUTEX_INITIALIZER;

/* Pipelines built with root-signature translation vs. falling back to per-draw descriptors (logged every 200). */
void bd12_root_push_count(int graphics, int used)
{
    static uint64_t counts[2][2];
    const uint64_t total = __atomic_add_fetch(&counts[graphics][used], 1, __ATOMIC_RELAXED) +
                           __atomic_load_n(&counts[graphics][!used], __ATOMIC_RELAXED);
    if (total == 1 || total % 200 == 0)
        fprintf(stderr, "[BD12] root-push %s pipelines: %llu of %llu\n", graphics ? "graphics" : "compute",
                (unsigned long long)__atomic_load_n(&counts[graphics][1], __ATOMIC_RELAXED),
                (unsigned long long)total);
}

int bd12_root_push_enabled(BD12Device *device)
{
    static int value = -1;
    if (value < 0) {
        const char *on = getenv("BEER_BD12_ROOT_PUSH");
        value = on && *on && strcmp(on, "0") != 0;
    }
    return value && device && device->modern && device->modern->descriptor_indexing &&
           device->modern->buffer_device_address;
}

static uint64_t fnv_words(uint64_t h, const uint32_t *words, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        for (int b = 0; b < 4; ++b) { h ^= (words[i] >> (b * 8)) & 0xffu; h *= 1099511628211ull; }
    }
    return h;
}

static void build_layout(BD12RootSignature *rs)
{
    rs->root_layout_state = -1;
    uint32_t range_total = 0;
    for (uint32_t i = 0; i < rs->parameter_count; ++i)
        if (rs->parameters[i].type == BD12_ROOT_TABLE) range_total += rs->parameters[i].range_count;
    BeerD3D12RootLayout *layout = calloc(1, sizeof(*layout));
    BeerD3D12RootRange *ranges = calloc(range_total ? range_total : 1, sizeof(*ranges));
    if (!layout || !ranges || rs->static_count > 64) { free(layout); free(ranges); return; }
    uint32_t descriptors = 0, words = 0, next_range = 0;
    uint64_t h = 1469598103934665603ull;
    /* Descriptor-buffer heaps (bd12_heapdb.c) need the mutable-heap binding model. */
    layout->heap_model = rs->device && rs->device->db ? BEER_HEAP_MODEL_MUTABLE : BEER_HEAP_MODEL_CLASSES;
    {
        const uint32_t model[2] = { 0x6d6f646cu, layout->heap_model };
        h = fnv_words(h, model, 2);
    }
    layout->param_count = rs->parameter_count;
    for (uint32_t i = 0; i < rs->parameter_count; ++i) {
        const BD12RootParameter *p = &rs->parameters[i];
        BeerD3D12RootParam *out = &layout->params[i];
        out->type = p->type;
        out->visibility = p->visibility;
        out->shader_register = p->shader_register;
        out->space = p->space;
        out->dword_count = p->dword_count;
        switch (p->type) {
        case BD12_ROOT_TABLE: {
            out->push_index = words++;
            out->ranges = &ranges[next_range];
            out->range_count = p->range_count;
            uint32_t running = 0;
            for (uint32_t r = 0; r < p->range_count; ++r) {
                const BD12DescriptorRange *src = &p->ranges[r];
                BeerD3D12RootRange *dst = &ranges[next_range++];
                const uint32_t offset = src->offset == BD12_RANGE_APPEND ? running : src->offset;
                *dst = (BeerD3D12RootRange){ src->type, src->count, src->base_register, src->space, offset };
                running = src->count == UINT32_MAX ? offset : offset + src->count;
                h = fnv_words(h, (const uint32_t *)dst, sizeof(*dst) / 4);
            }
            break;
        }
        case BD12_ROOT_CONSTANTS: out->push_index = words; words += p->dword_count; break;
        default: out->push_index = descriptors++; break;
        }
        const uint32_t key[7] = { out->type, out->visibility, out->shader_register, out->space, out->dword_count,
                                  out->range_count, out->push_index };
        h = fnv_words(h, key, 7);
    }
    layout->zero_word = words++;
    layout->descriptor_count = descriptors;
    layout->word_count = words;
    layout->static_count = rs->static_count;
    for (uint32_t i = 0; i < rs->static_count; ++i) {
        layout->static_register[i] = rs->statics[i].reg;
        layout->static_space[i] = rs->statics[i].space;
        const uint32_t key[2] = { rs->statics[i].reg, rs->statics[i].space };
        h = fnv_words(h, key, 2);
    }
    layout->hash = h ? h : 1;
    const uint32_t bytes = descriptors * 8 + words * 4;
    const uint32_t limit = rs->device && rs->device->db ? bd12_db_push_size(rs->device) :
                           rs->device && rs->device->max_push_constants ? rs->device->max_push_constants : 128;
    if (bytes > limit || bytes > sizeof(((BD12CmdList *)0)->root_push_last[0])) {
        static int logged;
        if (logged++ < 4)
            fprintf(stderr, "[BD12] root signature needs %u push-constant bytes (limit %u): per-draw descriptors\n",
                    bytes, limit);
        free(layout);
        free(ranges);
        return;
    }
    rs->root_layout = layout;
    rs->root_ranges = ranges;
    rs->root_push_bytes = bytes;
    rs->root_layout_state = 1;
}

const BeerD3D12RootLayout *bd12_root_layout(BD12RootSignature *rs)
{
    if (!rs) return NULL;
    int state = __atomic_load_n(&rs->root_layout_state, __ATOMIC_ACQUIRE);
    if (!state) {
        pthread_mutex_lock(&g_layout_lock);
        if (!rs->root_layout_state) build_layout(rs);
        pthread_mutex_unlock(&g_layout_lock);
        state = __atomic_load_n(&rs->root_layout_state, __ATOMIC_ACQUIRE);
    }
    return state > 0 ? rs->root_layout : NULL;
}

/* The push block for `state`: root descriptor addresses, then the words (tables' first heap index, constants). */
static void build_push(BD12CmdList *list, const BD12BindState *state, uint8_t *out, uint32_t bytes)
{
    BD12RootSignature *rs = state->root_signature;
    const BeerD3D12RootLayout *layout = rs->root_layout;
    memset(out, 0, bytes);
    uint32_t *words = (uint32_t *)(out + layout->descriptor_count * 8);
    for (uint32_t i = 0; i < layout->param_count; ++i) {
        const BeerD3D12RootParam *p = &layout->params[i];
        const BD12RootArgument *arg = &state->arguments[i];
        switch (p->type) {
        case BEER_ROOT_TABLE: {
            const int samplers = p->range_count && p->ranges[0].type == BEER_D3D12_SHADER_DESCRIPTOR_SAMPLER;
            const BD12DescriptorHeap *heap = list->heaps[samplers ? 1 : 0];
            if (heap && arg->value >= heap->gpu_base &&
                arg->value < heap->gpu_base + (uint64_t)heap->count * BD12_DESCRIPTOR_INCREMENT)
                words[p->push_index] = (uint32_t)((arg->value - heap->gpu_base) / BD12_DESCRIPTOR_INCREMENT);
            break;
        }
        case BEER_ROOT_CONSTANTS:
            memcpy(&words[p->push_index], arg->constants, p->dword_count * 4u);
            break;
        default:
            memcpy(out + p->push_index * 8, &arg->value, 8);
            break;
        }
    }
}

int bd12_cmd_push_root_arguments(BD12CmdList *list, BD12BindState *state, VkPipelineBindPoint point,
                                 VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t push_bytes,
                                 uint64_t layout_hash, const BD12DbLayout *db_layout)
{
    const BD12RootSignature *rs = state->root_signature;
    if (!rs || !rs->root_layout || rs->root_layout->hash != layout_hash || rs->root_push_bytes != push_bytes)
        return 0;   /* the bound signature's layout differs from the one the pipeline was translated for */
    const int which = point == VK_PIPELINE_BIND_POINT_COMPUTE;
    uint64_t key;
    if (db_layout) {
        /* Every descriptor-buffer layout has the same push range, so pushed arguments survive pipeline and root
         * signature changes: one compatibility class (key 1). */
        bd12_db_bind(list, point, db_layout);
        layout = bd12_db_pipeline_layout(db_layout);
        stages = bd12_db_push_stages(list->device);
        key = 1;
    } else {
        if (!bd12_mirror_bind(list, point, layout)) return 0;   /* bd12_mirror_prepare ran before rendering began */
        key = (uint64_t)(uintptr_t)layout;
        if (key == 1) key = 2;
    }
    uint8_t block[256];
    build_push(list, state, block, push_bytes);
    if (list->root_push_key[which] == key && list->root_push_size[which] == push_bytes &&
        !memcmp(list->root_push_last[which], block, push_bytes))
        return 1;
    list->device->vk.CmdPushConstants(list->command_buffer, layout, stages, 0, push_bytes, block);
    memcpy(list->root_push_last[which], block, push_bytes);
    list->root_push_key[which] = key;
    list->root_push_size[which] = push_bytes;
    return 1;
}
