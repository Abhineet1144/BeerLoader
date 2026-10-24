#include "win32_unwind.h"

#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

#define BEER_FUNCTION_TABLE_CAPACITY 64u
#define BEER_FUNCTION_TABLE_FIXED 1u
#define BEER_FUNCTION_TABLE_CALLBACK 2u

typedef struct BeerDynamicFunctionTable {
    _Atomic unsigned kind;
    const BeerRuntimeFunction *table;
    uint32_t count;
    uint32_t length;
    uint64_t base;
    uint64_t identifier;
    BeerFunctionTableCallback callback;
    void *context;
} BeerDynamicFunctionTable;

static BeerDynamicFunctionTable dynamic_tables[BEER_FUNCTION_TABLE_CAPACITY];

static int function_entry_contains(const BeerRuntimeFunction *entry, uint64_t relative_pc)
{
    return entry && entry->begin_address < entry->end_address &&
           relative_pc >= entry->begin_address && relative_pc < entry->end_address;
}

int beer_add_function_table(const BeerRuntimeFunction *table, uint32_t count,
                            uint64_t base_address)
{
    if (!table || !count || !base_address) return 0;
    for (unsigned i = 0; i < BEER_FUNCTION_TABLE_CAPACITY; ++i) {
        unsigned expected = 0;
        if (!atomic_compare_exchange_strong_explicit(&dynamic_tables[i].kind, &expected,
                                                      UINT32_MAX, memory_order_acq_rel,
                                                      memory_order_relaxed))
            continue;
        dynamic_tables[i].table = table;
        dynamic_tables[i].count = count;
        dynamic_tables[i].base = base_address;
        dynamic_tables[i].length = 0;
        dynamic_tables[i].identifier = 0;
        dynamic_tables[i].callback = NULL;
        dynamic_tables[i].context = NULL;
        atomic_store_explicit(&dynamic_tables[i].kind, BEER_FUNCTION_TABLE_FIXED,
                              memory_order_release);
        return 1;
    }
    return 0;
}

int beer_delete_function_table(const BeerRuntimeFunction *table)
{
    if (!table) return 0;
    uint64_t key = (uint64_t)(uintptr_t)table;
    for (unsigned i = 0; i < BEER_FUNCTION_TABLE_CAPACITY; ++i) {
        unsigned kind = atomic_load_explicit(&dynamic_tables[i].kind,
                                             memory_order_acquire);
        int matches = (kind == BEER_FUNCTION_TABLE_FIXED &&
                       dynamic_tables[i].table == table) ||
                      (kind == BEER_FUNCTION_TABLE_CALLBACK &&
                       dynamic_tables[i].identifier == key);
        if (!matches) continue;
        atomic_store_explicit(&dynamic_tables[i].kind, 0, memory_order_release);
        return 1;
    }
    return 0;
}

int beer_install_function_table_callback(uint64_t table_identifier,
                                         uint64_t base_address,
                                         uint32_t length,
                                         BeerFunctionTableCallback callback,
                                         void *context)
{
    /* Windows reserves the low two identifier bits to distinguish callback
     * tables from ordinary aligned table pointers. */
    if ((table_identifier & 3u) != 3u || !base_address || !length || !callback)
        return 0;
    for (unsigned i = 0; i < BEER_FUNCTION_TABLE_CAPACITY; ++i) {
        unsigned expected = 0;
        if (!atomic_compare_exchange_strong_explicit(&dynamic_tables[i].kind, &expected,
                                                      UINT32_MAX, memory_order_acq_rel,
                                                      memory_order_relaxed))
            continue;
        dynamic_tables[i].table = NULL;
        dynamic_tables[i].count = 0;
        dynamic_tables[i].base = base_address;
        dynamic_tables[i].length = length;
        dynamic_tables[i].identifier = table_identifier;
        dynamic_tables[i].callback = callback;
        dynamic_tables[i].context = context;
        atomic_store_explicit(&dynamic_tables[i].kind, BEER_FUNCTION_TABLE_CALLBACK,
                              memory_order_release);
        return 1;
    }
    return 0;
}

const BeerRuntimeFunction *beer_lookup_dynamic_function_entry(uint64_t control_pc,
                                                               uint64_t *image_base)
{
    for (unsigned slot = 0; slot < BEER_FUNCTION_TABLE_CAPACITY; ++slot) {
        unsigned kind = atomic_load_explicit(&dynamic_tables[slot].kind,
                                             memory_order_acquire);
        BeerDynamicFunctionTable *registration = &dynamic_tables[slot];
        if (kind == BEER_FUNCTION_TABLE_FIXED) {
            if (control_pc < registration->base) continue;
            uint64_t relative_pc = control_pc - registration->base;
            /* Registered tables are normally sorted, but accepting an unsorted
             * table costs little and is friendlier to generated/protected code. */
            for (uint32_t i = 0; i < registration->count; ++i) {
                if (!function_entry_contains(&registration->table[i], relative_pc))
                    continue;
                if (image_base) *image_base = registration->base;
                return &registration->table[i];
            }
        } else if (kind == BEER_FUNCTION_TABLE_CALLBACK) {
            if (control_pc < registration->base ||
                control_pc - registration->base >= registration->length)
                continue;
            const BeerRuntimeFunction *entry =
                registration->callback(control_pc, registration->context);
            if (!function_entry_contains(entry, control_pc - registration->base))
                continue;
            if (image_base) *image_base = registration->base;
            return entry;
        }
    }
    return NULL;
}

void beer_reset_dynamic_function_tables(void)
{
    for (unsigned i = 0; i < BEER_FUNCTION_TABLE_CAPACITY; ++i)
        atomic_store_explicit(&dynamic_tables[i].kind, 0, memory_order_release);
}

#define UNW_FLAG_EHANDLER 1u
#define UNW_FLAG_UHANDLER 2u
#define UNW_FLAG_CHAININFO 4u

#define UWOP_PUSH_NONVOL 0u
#define UWOP_ALLOC_LARGE 1u
#define UWOP_ALLOC_SMALL 2u
#define UWOP_SET_FPREG 3u
#define UWOP_SAVE_NONVOL 4u
#define UWOP_SAVE_NONVOL_FAR 5u
#define UWOP_SAVE_XMM128 8u
#define UWOP_SAVE_XMM128_FAR 9u
#define UWOP_PUSH_MACHFRAME 10u

static const uint16_t gp_offsets[16] = {
    0x78, 0x80, 0x88, 0x90, 0x98, 0xa0, 0xa8, 0xb0,
    0xb8, 0xc0, 0xc8, 0xd0, 0xd8, 0xe0, 0xe8, 0xf0
};

static uint64_t get_gp(const uint8_t *context, unsigned reg)
{
    uint64_t value;
    memcpy(&value, context + gp_offsets[reg], sizeof(value));
    return value;
}

static void set_gp(uint8_t *context, unsigned reg, uint64_t value)
{
    memcpy(context + gp_offsets[reg], &value, sizeof(value));
}

static uint16_t read_u16(const void *value)
{
    uint16_t result;
    memcpy(&result, value, sizeof(result));
    return result;
}

static uint32_t read_u32(const void *value)
{
    uint32_t result;
    memcpy(&result, value, sizeof(result));
    return result;
}

static uint64_t read_u64(const void *value)
{
    uint64_t result;
    memcpy(&result, value, sizeof(result));
    return result;
}

uint64_t beer_virtual_unwind(uint32_t handler_type, uint64_t image_base,
                            uint64_t control_pc,
                            const BeerRuntimeFunction *function_entry,
                            void *context_record, uint64_t *handler_data,
                            uint64_t *establisher_frame, int *ok)
{
    (void)control_pc;
    if (ok) *ok = 0;
    if (handler_data) *handler_data = 0;
    if (!image_base || !function_entry || !context_record) return 0;

    uint8_t saved[0x4d0];
    uint8_t *context = context_record;
    memcpy(saved, context, sizeof(saved));
    const BeerRuntimeFunction *entry = function_entry;
    uint64_t handler = 0;
    /* The establisher frame is the frame's own stack pointer as the function
     * body sees it: the frame register minus its scaled offset when the
     * function has one, otherwise RSP at the unwind point, i.e. BEFORE the
     * unwind codes are applied (this is what setjmp records as the jump
     * buffer's Frame and what C++/SEH language handlers expect). */
    uint64_t body_frame;
    {
        const uint8_t *first_info =
            (const uint8_t *)(uintptr_t)(image_base + function_entry->unwind_data);
        const unsigned first_frame_reg = first_info[3] & 15u;
        const unsigned first_frame_offset = first_info[3] >> 4;
        body_frame = first_frame_reg
            ? get_gp(context, first_frame_reg) - (uint64_t)first_frame_offset * 16
            : get_gp(context, 4);
    }

    for (unsigned chain = 0; chain < 16; ++chain) {
        if (!entry->unwind_data) goto fail;
        const uint8_t *info = (const uint8_t *)(uintptr_t)(image_base + entry->unwind_data);
        unsigned version = info[0] & 7u;
        unsigned flags = info[0] >> 3;
        unsigned count = info[2];
        unsigned frame_reg = info[3] & 15u;
        unsigned frame_offset = info[3] >> 4;
        if (version != 1 || (flags & ~(UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER |
                                      UNW_FLAG_CHAININFO))) goto fail;

        uint64_t rsp = get_gp(context, 4);
        const uint8_t *codes = info + 4;
        for (unsigned index = 0; index < count;) {
            unsigned op = codes[index * 2 + 1] & 15u;
            unsigned op_info = codes[index * 2 + 1] >> 4;
            uint64_t offset;
            switch (op) {
            case UWOP_PUSH_NONVOL:
                if (op_info >= 16) goto fail;
                set_gp(context, op_info, read_u64((const void *)(uintptr_t)rsp));
                rsp += 8;
                index++;
                break;
            case UWOP_ALLOC_LARGE:
                if (op_info == 0) {
                    if (index + 1 >= count) goto fail;
                    rsp += (uint64_t)read_u16(codes + (index + 1) * 2) * 8;
                    index += 2;
                } else if (op_info == 1) {
                    if (index + 2 >= count) goto fail;
                    rsp += read_u32(codes + (index + 1) * 2);
                    index += 3;
                } else goto fail;
                break;
            case UWOP_ALLOC_SMALL:
                rsp += (uint64_t)op_info * 8 + 8;
                index++;
                break;
            case UWOP_SET_FPREG:
                if (!frame_reg) goto fail;
                rsp = get_gp(context, frame_reg) - (uint64_t)frame_offset * 16;
                index++;
                break;
            case UWOP_SAVE_NONVOL:
                if (op_info >= 16 || index + 1 >= count) goto fail;
                offset = (uint64_t)read_u16(codes + (index + 1) * 2) * 8;
                set_gp(context, op_info,
                       read_u64((const void *)(uintptr_t)(rsp + offset)));
                index += 2;
                break;
            case UWOP_SAVE_NONVOL_FAR:
                if (op_info >= 16 || index + 2 >= count) goto fail;
                offset = read_u32(codes + (index + 1) * 2);
                set_gp(context, op_info,
                       read_u64((const void *)(uintptr_t)(rsp + offset)));
                index += 3;
                break;
            case UWOP_SAVE_XMM128:
                if (op_info >= 16 || index + 1 >= count) goto fail;
                offset = (uint64_t)read_u16(codes + (index + 1) * 2) * 16;
                memcpy(context + 0x1a0 + op_info * 16,
                       (const void *)(uintptr_t)(rsp + offset), 16);
                index += 2;
                break;
            case UWOP_SAVE_XMM128_FAR:
                if (op_info >= 16 || index + 2 >= count) goto fail;
                offset = read_u32(codes + (index + 1) * 2);
                memcpy(context + 0x1a0 + op_info * 16,
                       (const void *)(uintptr_t)(rsp + offset), 16);
                index += 3;
                break;
            case UWOP_PUSH_MACHFRAME: {
                uint64_t error_offset = op_info ? 8 : 0;
                if (op_info > 1) goto fail;
                uint64_t machine = rsp + error_offset;
                uint64_t rip = read_u64((const void *)(uintptr_t)machine);
                uint64_t final_rsp = read_u64((const void *)(uintptr_t)(machine + 24));
                memcpy(context + 0xf8, &rip, sizeof(rip));
                set_gp(context, 4, final_rsp);
                if (establisher_frame) *establisher_frame = final_rsp;
                if (ok) *ok = 1;
                return 0;
            }
            default:
                goto fail;
            }
            set_gp(context, 4, rsp);
        }

        unsigned padded = (count + 1u) & ~1u;
        const uint8_t *tail = codes + padded * 2u;
        if (flags & UNW_FLAG_CHAININFO) {
            if (flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) goto fail;
            entry = (const BeerRuntimeFunction *)tail;
            continue;
        }
        /* Only the handler class the caller asked for (EHANDLER while
         * searching, UHANDLER while unwinding) is reported. */
        if (flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) {
            if (!(flags & handler_type)) break;
            uint32_t handler_rva = read_u32(tail);
            handler = image_base + handler_rva;
            if (handler_data) *handler_data = (uint64_t)(uintptr_t)(tail + 4);
        }
        break;
    }

    {
        uint64_t rsp = get_gp(context, 4);
        if (establisher_frame) *establisher_frame = body_frame;
        uint64_t rip = read_u64((const void *)(uintptr_t)rsp);
        rsp += 8;
        memcpy(context + 0xf8, &rip, sizeof(rip));
        set_gp(context, 4, rsp);
    }
    if (ok) *ok = 1;
    return handler;

fail:
    memcpy(context, saved, sizeof(saved));
    return 0;
}
