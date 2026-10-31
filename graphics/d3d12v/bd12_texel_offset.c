#define _GNU_SOURCE
#include "bd12.h"

#include <stdlib.h>
#include <string.h>

/* Texel-buffer view offsets for devices with coarse texel-buffer alignment.
 *
 * D3D12 buffer views may start at any element; Vulkan texel-buffer views must start at a multiple of
 * minTexelBufferOffsetAlignment (64 bytes on Adreno) unless the device has single-texel alignment. On such devices
 * a misaligned view is created at the aligned-down offset and the remaining whole elements are added to every
 * coordinate the shader uses with it. The per-binding element offsets live in a read-only storage buffer at an
 * extra set-0 binding, filled for every draw/dispatch (bd12_bind.c).
 *
 * bd12_spirv_texel_offsets() rewrites a translated shader: for each set-0 texel-buffer variable it adds the offset
 * of its slot (slot base + array index) to the coordinate of OpImageFetch / OpImageRead / OpImageWrite /
 * OpImageTexelPointer and subtracts it from OpImageQuerySize. Devices that do not need this never run it. */

enum {
    OP_EXTENSION = 10, OP_EXT_INST_IMPORT = 11, OP_MEMORY_MODEL = 14, OP_ENTRY_POINT = 15, OP_CAPABILITY = 17,
    OP_TYPE_INT = 21, OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_CONSTANT = 43,
    OP_FUNCTION = 54, OP_VARIABLE = 59, OP_IMAGE_TEXEL_POINTER = 60, OP_LOAD = 61, OP_ACCESS_CHAIN = 65,
    OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_COPY_OBJECT = 83,
    OP_IMAGE_FETCH = 95, OP_IMAGE_READ = 98, OP_IMAGE_WRITE = 99, OP_IMAGE_QUERY_SIZE = 104, OP_BITCAST = 124,
    OP_IADD = 128, OP_ISUB = 130,
    DEC_BLOCK = 2, DEC_ARRAY_STRIDE = 6, DEC_NON_WRITABLE = 24, DEC_BINDING = 33, DEC_DESCRIPTOR_SET = 34,
    DEC_OFFSET = 35, SC_STORAGE_BUFFER = 12
};

typedef struct { uint32_t *w; size_t n, cap; } Words;

static int put(Words *o, uint32_t v)
{
    if (o->n == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 4096;
        uint32_t *grown = realloc(o->w, cap * sizeof(*grown));
        if (!grown) return 0;
        o->w = grown;
        o->cap = cap;
    }
    o->w[o->n++] = v;
    return 1;
}

static int put_op(Words *o, uint32_t op, uint32_t count, const uint32_t *operands)
{
    if (!put(o, ((count + 1) << 16) | op)) return 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!put(o, operands[i])) return 0;
    return 1;
}

/* Opcodes whose first two operands are <result type> <result id>. */
static int has_result_type(uint32_t op)
{
    if (op == 12 || (op >= 41 && op <= 52) || (op >= 54 && op <= 57) || op == 59 || op == 60 || op == 61) return 1;
    if ((op >= 65 && op <= 70) || (op >= 77 && op <= 83)) return 1;
    if (op >= 86 && op <= 107 && op != OP_IMAGE_WRITE) return 1;
    return (op >= 109 && op <= 202) || op == 245;
}

typedef struct { uint32_t slot_base, index_id, tracked, table; } Tracked;   /* table: 0 = set 0, 1 = heap mirror */

uint32_t *bd12_spirv_texel_offsets(const uint32_t *in, size_t in_bytes, const uint32_t *bindings,
                                   const uint32_t *slot_base, uint32_t binding_count, uint32_t offsets_binding,
                                   int bindless, size_t *out_bytes)
{
    const size_t words = in_bytes / 4;
    if (words < 5 || in[0] != 0x07230203u) return NULL;
    const uint32_t bound = in[3], version = in[1];
    uint32_t *type_of = calloc(bound, sizeof(*type_of));
    uint32_t *binding_of = calloc(bound, sizeof(*binding_of));   /* binding + 1 */
    uint32_t *set_of = calloc(bound, sizeof(*set_of));           /* set + 1 */
    uint8_t *is_int = calloc(bound, 1);                          /* 1 = 32-bit unsigned, 2 = 32-bit signed */
    Tracked *tracked = calloc(bound, sizeof(*tracked));
    Words out = {0};
    uint32_t *consts = NULL, *result = NULL;
    if (!type_of || !binding_of || !set_of || !is_int || !tracked) goto done;

    /* Pass 1: decorations, integer types, result types; existing extension. */
    uint32_t uint_type = 0, slots = 0;
    int has_sb_ext = 0, any_use = 0;
    for (uint32_t i = 0; i < binding_count; ++i)
        if (slot_base[i] + 1 > slots) slots = slot_base[i] + 1;
    for (size_t at = 5; at < words;) {
        const uint32_t op = in[at] & 0xffffu, wc = in[at] >> 16;
        if (!wc || at + wc > words) goto done;
        const uint32_t *a = in + at + 1;
        if (op == OP_DECORATE && wc >= 4 && a[0] < bound) {
            if (a[1] == DEC_BINDING) binding_of[a[0]] = a[2] + 1;
            if (a[1] == DEC_DESCRIPTOR_SET) set_of[a[0]] = a[2] + 1;
        } else if (op == OP_TYPE_INT && wc >= 4 && a[0] < bound && a[1] == 32) {
            is_int[a[0]] = a[2] ? 2 : 1;
            if (!a[2] && !uint_type) uint_type = a[0];
        } else if (op == OP_EXTENSION && wc >= 2 &&
                   !strncmp((const char *)a, "SPV_KHR_storage_buffer_storage_class", (wc - 1) * 4)) {
            has_sb_ext = 1;
        }
        if (has_result_type(op) && wc >= 3 && a[1] < bound && !type_of[a[1]]) type_of[a[1]] = a[0];
        at += wc;
    }
    /* Set-0 variables of the given texel-buffer bindings; with `bindless`, also the heap mirror's texel arrays
     * (set 1, bindings 2 and 3), whose offsets table is indexed by heap slot. */
    int use_table[2] = { 0, 0 };
    for (size_t at = 5; at < words;) {
        const uint32_t op = in[at] & 0xffffu, wc = in[at] >> 16;
        const uint32_t *a = in + at + 1;
        if (op == OP_VARIABLE && wc >= 4 && a[1] < bound && binding_of[a[1]] && set_of[a[1]] <= 1) {
            for (uint32_t i = 0; i < binding_count; ++i)
                if (bindings[i] + 1 == binding_of[a[1]]) {
                    tracked[a[1]] = (Tracked){ slot_base[i], 0, 1, 0 };
                    use_table[0] = 1;
                    break;
                }
        } else if (bindless && op == OP_VARIABLE && wc >= 4 && a[1] < bound && set_of[a[1]] == 2 &&
                   (binding_of[a[1]] == 3 || binding_of[a[1]] == 4)) {
            tracked[a[1]] = (Tracked){ 0, 0, 1, 1 };
            use_table[1] = 1;
        }
        at += wc;
    }
    if (!use_table[0] && !use_table[1]) goto done;

    /* Fresh ids for the offset table declarations and per-use temporaries. */
    uint32_t next = bound;
    const uint32_t new_uint = uint_type ? 0 : next++;
    if (!uint_type) uint_type = new_uint;
    const uint32_t rt_array = next++, block = next++, ptr_block = next++, ptr_uint = next++;
    const uint32_t tables[2] = { use_table[0] ? next++ : 0, use_table[1] ? next++ : 0 };
    consts = calloc(slots + 1, sizeof(*consts));
    if (!consts) goto done;
    for (uint32_t i = 0; i <= slots; ++i) consts[i] = next++;   /* consts[0] = 0u (member index), consts[1+k] = k */

    /* Pass 2: emit, rewriting uses inside functions. */
    int extension_done = version >= 0x10300 || has_sb_ext, annotations_done = 0, globals_done = 0;
    for (size_t at = 0; at < 5; ++at) if (!put(&out, in[at])) goto done;
    for (size_t at = 5; at < words;) {
        const uint32_t op = in[at] & 0xffffu, wc = in[at] >> 16;
        const uint32_t *a = in + at + 1;
        if (!extension_done && (op == OP_EXT_INST_IMPORT || op == OP_MEMORY_MODEL)) {
            static const char name[] = "SPV_KHR_storage_buffer_storage_class";
            uint32_t text[10] = {0};
            memcpy(text, name, sizeof(name));
            if (!put_op(&out, OP_EXTENSION, (sizeof(name) + 3) / 4, text)) goto done;
            extension_done = 1;
        }
        if (!annotations_done && ((op >= 19 && op <= 39) || op == OP_CONSTANT || op == OP_VARIABLE)) {
            const uint32_t d[][4] = { { rt_array, DEC_ARRAY_STRIDE, 4 }, { block, DEC_BLOCK } };
            if (!put_op(&out, OP_DECORATE, 3, d[0]) || !put_op(&out, OP_DECORATE, 2, d[1])) goto done;
            for (int t = 0; t < 2; ++t) {
                if (!tables[t]) continue;
                const uint32_t ds[] = { tables[t], DEC_DESCRIPTOR_SET, (uint32_t)t },
                               bd[] = { tables[t], DEC_BINDING, t ? BD12_MIRROR_OFFSETS_BINDING : offsets_binding };
                if (!put_op(&out, OP_DECORATE, 3, ds) || !put_op(&out, OP_DECORATE, 3, bd)) goto done;
            }
            const uint32_t m0[] = { block, 0, DEC_OFFSET, 0 }, m1[] = { block, 0, DEC_NON_WRITABLE };
            if (!put_op(&out, OP_MEMBER_DECORATE, 4, m0) || !put_op(&out, OP_MEMBER_DECORATE, 3, m1)) goto done;
            annotations_done = 1;
        }
        if (!globals_done && op == OP_FUNCTION) {
            if (new_uint) {
                const uint32_t t[] = { new_uint, 32, 0 };
                if (!put_op(&out, OP_TYPE_INT, 3, t)) goto done;
            }
            const uint32_t ra[] = { rt_array, uint_type }, st[] = { block, rt_array },
                           pb[] = { ptr_block, SC_STORAGE_BUFFER, block }, pu[] = { ptr_uint, SC_STORAGE_BUFFER, uint_type };
            if (!put_op(&out, OP_TYPE_RUNTIME_ARRAY, 2, ra) || !put_op(&out, OP_TYPE_STRUCT, 2, st) ||
                !put_op(&out, OP_TYPE_POINTER, 3, pb) || !put_op(&out, OP_TYPE_POINTER, 3, pu))
                goto done;
            for (int t = 0; t < 2; ++t) {
                const uint32_t var[] = { ptr_block, tables[t], SC_STORAGE_BUFFER };
                if (tables[t] && !put_op(&out, OP_VARIABLE, 3, var)) goto done;
            }
            for (uint32_t i = 0; i <= slots; ++i) {
                const uint32_t c[] = { uint_type, consts[i], i ? i - 1 : 0 };
                if (!put_op(&out, OP_CONSTANT, 3, c)) goto done;
            }
            globals_done = 1;
        }
        if (op == OP_ENTRY_POINT && version >= 0x10400) {   /* SPIR-V 1.4+: every global is in the interface */
            const uint32_t extra = (tables[0] != 0) + (tables[1] != 0);
            if (!put(&out, ((wc + extra) << 16) | op)) goto done;
            for (uint32_t i = 0; i < wc - 1; ++i) if (!put(&out, a[i])) goto done;
            for (int t = 0; t < 2; ++t) if (tables[t] && !put(&out, tables[t])) goto done;
            at += wc;
            continue;
        }
        /* Propagate tracked pointers/images. */
        if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && wc >= 5 && a[2] < bound &&
            tracked[a[2]].tracked && !tracked[a[2]].index_id && a[1] < bound)
            tracked[a[1]] = (Tracked){ tracked[a[2]].slot_base, a[3], 1, tracked[a[2]].table };
        else if ((op == OP_LOAD || op == OP_COPY_OBJECT) && wc >= 4 && a[2] < bound && tracked[a[2]].tracked &&
                 a[1] < bound)
            tracked[a[1]] = tracked[a[2]];

        uint32_t image = 0, coord_pos = 0;   /* coord_pos: operand index of the coordinate */
        if ((op == OP_IMAGE_FETCH || op == OP_IMAGE_READ || op == OP_IMAGE_TEXEL_POINTER) && wc >= 5) {
            image = a[2];
            coord_pos = 3;
        } else if (op == OP_IMAGE_WRITE && wc >= 4) {
            image = a[0];
            coord_pos = 1;
        } else if (op == OP_IMAGE_QUERY_SIZE && wc >= 4) {
            image = a[2];
        }
        const Tracked *t = image < bound && image ? &tracked[image] : NULL;
        const uint32_t value_type = coord_pos ? (a[coord_pos] < bound ? type_of[a[coord_pos]] : 0) : a[0];
        if (!t || !t->tracked || value_type >= bound || !is_int[value_type]) {
            for (uint32_t i = 0; i < wc; ++i) if (!put(&out, in[at + i])) goto done;
            at += wc;
            continue;
        }
        any_use = 1;
        /* slot = base + index (set 0) or the heap index itself (mirror); offset = table[slot] as the coordinate's
         * integer type. */
        uint32_t slot = consts[1 + t->slot_base];
        if (t->index_id) {
            uint32_t index = t->index_id;
            if (index < bound && type_of[index] != uint_type) {
                const uint32_t b[] = { uint_type, next, index };
                if (!put_op(&out, OP_BITCAST, 3, b)) goto done;
                index = next++;
            }
            if (t->table) {
                slot = index;
            } else {
                const uint32_t add[] = { uint_type, next, consts[1 + t->slot_base], index };
                if (!put_op(&out, OP_IADD, 4, add)) goto done;
                slot = next++;
            }
        }
        const uint32_t chain[] = { ptr_uint, next, tables[t->table], consts[0], slot };
        if (!put_op(&out, OP_ACCESS_CHAIN, 5, chain)) goto done;
        const uint32_t load[] = { uint_type, next + 1, next };
        if (!put_op(&out, OP_LOAD, 3, load)) goto done;
        uint32_t offset = next + 1;
        next += 2;
        if (value_type != uint_type) {
            const uint32_t b[] = { value_type, next, offset };
            if (!put_op(&out, OP_BITCAST, 3, b)) goto done;
            offset = next++;
        }
        if (coord_pos) {
            const uint32_t add[] = { value_type, next, a[coord_pos], offset };
            if (!put_op(&out, OP_IADD, 4, add)) goto done;
            if (!put(&out, in[at])) goto done;
            for (uint32_t i = 0; i < wc - 1; ++i) if (!put(&out, i == coord_pos ? next : a[i])) goto done;
            ++next;
        } else {   /* OpImageQuerySize: produce the original result id as size - offset */
            const uint32_t raw = next++;
            if (!put(&out, in[at]) || !put(&out, a[0]) || !put(&out, raw)) goto done;
            for (uint32_t i = 2; i < wc - 1; ++i) if (!put(&out, a[i])) goto done;
            const uint32_t sub[] = { value_type, a[1], raw, offset };
            if (!put_op(&out, OP_ISUB, 4, sub)) goto done;
        }
        at += wc;
    }
    if (!any_use || !globals_done || !annotations_done) goto done;
    out.w[3] = next;
    result = out.w;
    out.w = NULL;
    *out_bytes = out.n * 4;
done:
    free(out.w);
    free(consts);
    free(type_of);
    free(binding_of);
    free(set_of);
    free(is_int);
    free(tracked);
    return result;
}

int bd12_texel_offsets_apply(uint32_t **spirv, size_t *spirv_bytes, BD12ShaderBinding *bindings, uint32_t count,
                             uint32_t offsets_binding, uint32_t *next_slot)
{
    uint32_t numbers[256], bases[256], n = 0;
    const uint32_t first_slot = *next_slot;
    for (uint32_t i = 0; i < count && n < 256; ++i) {
        BD12ShaderBinding *b = &bindings[i];
        b->offset_slot = 0;
        if (!b->is_buffer || b->is_accel || b->bindless ||
            (b->type != BD12_RANGE_SRV && b->type != BD12_RANGE_UAV))
            continue;
        numbers[n] = b->binding;
        bases[n++] = *next_slot;
        b->offset_slot = 1 + *next_slot;
        *next_slot += b->count ? b->count : 1;
    }
    size_t bytes = 0;
    /* Heap-mirror texel arrays (bindless) are found by the pass itself; their table is BD12_MIRROR_OFFSETS_BINDING. */
    uint32_t *patched = bd12_spirv_texel_offsets(*spirv, *spirv_bytes, numbers, bases, n, offsets_binding, 1, &bytes);
    if (!patched) {   /* nothing to rewrite (or unparsable): the stage keeps exact view offsets */
        for (uint32_t i = 0; i < count; ++i) bindings[i].offset_slot = 0;
        *next_slot = first_slot;
        return 0;
    }
    free(*spirv);
    *spirv = patched;
    *spirv_bytes = bytes;
    return 1;
}
