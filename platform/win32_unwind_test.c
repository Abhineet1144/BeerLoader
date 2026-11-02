#include "win32_unwind.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t get64(const uint8_t *context, unsigned offset)
{
    uint64_t value;
    memcpy(&value, context + offset, 8);
    return value;
}

static void put64(uint8_t *context, unsigned offset, uint64_t value)
{
    memcpy(context + offset, &value, 8);
}

static BeerRuntimeFunction callback_entry = { 0x40, 0x80, 0x90 };

static void test_frame_register_and_dynamic_stack(void)
{
    uint8_t image[256] = {0};
    uint64_t stack[40] = {0};
    uint8_t context[0x4d0];
    uint64_t base = (uint64_t)(uintptr_t)image;
    BeerRuntimeFunction function = {0x10, 0x70, 0x80};
    uint8_t *info = image + 0x80;
    info[0] = 1;
    info[1] = 16;
    info[2] = 7;
    info[3] = 0x25; /* RBP = fixed RSP + 32 */
    info[4] = 16; info[5] = 0x68; /* save XMM6 at fixed base + 32 */
    info[6] = 2;
    info[8] = 12; info[9] = 0xc4; /* save R12 at fixed base + 16 */
    info[10] = 2;
    info[12] = 8; info[13] = 3; /* establish frame pointer */
    info[14] = 4; info[15] = 0x72; /* allocate 64 */
    info[16] = 1; info[17] = 0x50; /* push RBP */
    stack[18] = 0x12121212;
    stack[20] = 0x60606060;
    stack[21] = 0x61616161;
    stack[24] = 0xBEEF;
    stack[25] = 0x12345678;
    for (unsigned offset = 0; offset <= 17; ++offset) {
        memset(context, 0, sizeof(context));
        size_t index = offset < 1 ? 25 : offset < 4 ? 24 : 16;
        /* After the prologue, alloca moves RSP below the fixed allocation. */
        if (offset == 17) index = 0;
        put64(context, 0x98, (uint64_t)(uintptr_t)&stack[index]);
        put64(context, 0xa0, offset >= 8 ? (uint64_t)(uintptr_t)&stack[20] : 0xBEEF);
        put64(context, 0xd8, 0x7777);
        uint64_t frame = 0;
        int ok = 0;
        assert(!beer_virtual_unwind(0, base, base + function.begin_address + offset,
            &function, context, NULL, &frame, &ok));
        assert(ok && frame == (uint64_t)(uintptr_t)&stack[offset == 17 ? 16 : index]);
        assert(get64(context, 0x98) == (uint64_t)(uintptr_t)&stack[26]);
        assert(get64(context, 0xf8) == stack[25]);
        assert(get64(context, 0xa0) == stack[24]);
        assert(get64(context, 0xd8) == (offset >= 12 ? stack[18] : 0x7777));
        if (offset >= 16) {
            assert(get64(context, 0x1a0 + 6 * 16) == stack[20]);
            assert(get64(context, 0x1a0 + 6 * 16 + 8) == stack[21]);
        }
    }
}

static const BeerRuntimeFunction *__attribute__((ms_abi))
test_callback(uint64_t control_pc, void *context)
{
    uint64_t *seen = context;
    *seen = control_pc;
    return &callback_entry;
}

static void test_chained_prolog_and_malformed_metadata(void)
{
    uint8_t image[256] = {0};
    uint64_t base = (uint64_t)(uintptr_t)image;
    BeerRuntimeFunction function = {0x50, 0x70, 0x80};
    BeerRuntimeFunction parent = {0x10, 0x40, 0xa0};
    uint8_t *child_info = image + 0x80, *parent_info = image + 0xa0;
    child_info[0] = 1 | (4 << 3);
    child_info[1] = 4;
    child_info[2] = 2;
    child_info[4] = 4; child_info[5] = 0xc4; /* save R12 */
    child_info[6] = 2;
    memcpy(child_info + 8, &parent, sizeof(parent));
    parent_info[0] = 1 | (1 << 3);
    parent_info[1] = 4;
    parent_info[2] = 2;
    parent_info[4] = 4; parent_info[5] = 0x72; /* alloc 64 */
    parent_info[6] = 1; parent_info[7] = 0x30; /* push RBX */
    uint32_t handler_rva = 0xc0;
    memcpy(parent_info + 8, &handler_rva, sizeof(handler_rva));
    uint64_t stack[16] = {0};
    stack[2] = 0x1212; stack[8] = 0xBEEF; stack[9] = 0xFEED;
    uint8_t context[0x4d0];
    for (unsigned offset = 0; offset <= 5; ++offset) {
        memset(context, 0, sizeof(context));
        put64(context, 0x98, (uint64_t)(uintptr_t)stack);
        put64(context, 0xd8, 0x7777);
        int ok;
        uint64_t data, frame;
        uint64_t handler = beer_virtual_unwind(1, base, base + 0x50 + offset,
            &function, context, &data, &frame, &ok);
        assert(ok && frame == (uint64_t)(uintptr_t)stack);
        assert(handler == (offset <= 4 ? 0 : base + handler_rva));
        assert(data == (offset <= 4 ? 0 : base + 0xac));
        assert(get64(context, 0x98) == (uint64_t)(uintptr_t)&stack[10]);
        assert(get64(context, 0xf8) == stack[9]);
        assert(get64(context, 0x90) == stack[8]);
        assert(get64(context, 0xd8) == (offset >= 4 ? stack[2] : 0x7777));
    }
    uint8_t before[sizeof(context)];
    memcpy(before, context, sizeof(before));
    int ok;
    child_info[2] = 1; /* Truncated multi-slot operation, even when skipped. */
    assert(!beer_virtual_unwind(1, base, base + 0x50, &function, context, NULL, NULL, &ok));
    assert(!ok && !memcmp(before, context, sizeof(before)));
    child_info[2] = 0;
    memcpy(child_info + 4, &function, sizeof(function)); /* Cyclic chain */
    assert(!beer_virtual_unwind(1, base, base + 0x50, &function, context, NULL, NULL, &ok));
    assert(!ok && !memcmp(before, context, sizeof(before)));
}

int main(void)
{
    test_frame_register_and_dynamic_stack();
    test_chained_prolog_and_malformed_metadata();
    uint8_t image[256] = {0};
    BeerRuntimeFunction function = { 0x10, 0x40, 0x80 };
    uint8_t *unwind = image + 0x80;
    unwind[0] = 1 | (1 << 3); /* version 1, EHANDLER */
    unwind[1] = 5;
    unwind[2] = 4;
    unwind[3] = 0;
    /* Codes are ordered by descending prologue offset: restore the saved
     * register from the final RSP, release the allocation, then undo the push. */
    unwind[4] = 5; unwind[5] = (12 << 4) | 4; /* save r12, offset slot */
    unwind[6] = 2; unwind[7] = 0; /* offset 16 bytes */
    unwind[8] = 4; unwind[9] = (7 << 4) | 2; /* alloc 64 */
    unwind[10] = 1; unwind[11] = (3 << 4) | 0; /* push rbx */
    *(uint32_t *)(unwind + 12) = 0x60;
    *(uint32_t *)(unwind + 16) = 0xabcdef01;

    uint64_t stack[16] = {0};
    uint8_t context[0x4d0] = {0};
    put64(context, 0x98, (uint64_t)(uintptr_t)&stack[0]);
    stack[2] = 0x1212121212121212ull; /* saved r12 at pre-unwind rsp+16 */
    stack[8] = 0x3434343434343434ull; /* pushed rbx after 64-byte alloc */
    stack[9] = 0x5656565656565656ull; /* return address */

    uint64_t data = 0, frame = 0;
    int ok = 0;
    uint64_t handler = beer_virtual_unwind(1, (uint64_t)(uintptr_t)image,
                                            (uint64_t)(uintptr_t)image + 0x20,
                                            &function, context, &data, &frame, &ok);
    assert(ok);
    assert(handler == (uint64_t)(uintptr_t)image + 0x60);
    assert(data == (uint64_t)(uintptr_t)(unwind + 16));
    assert(get64(context, 0x90) == stack[8]);
    assert(get64(context, 0xd8) == stack[2]);
    assert(get64(context, 0xf8) == stack[9]);
    assert(get64(context, 0x98) == (uint64_t)(uintptr_t)&stack[10]);
    /* EstablisherFrame is the base of this function's fixed allocation, not
     * its caller return slot. HandlerType zero requests no handler at all. */
    assert(frame == (uint64_t)(uintptr_t)&stack[0]);

    memset(context, 0, sizeof(context));
    put64(context, 0x98, (uint64_t)(uintptr_t)&stack[0]);
    assert(!beer_virtual_unwind(0, (uint64_t)(uintptr_t)image,
        (uint64_t)(uintptr_t)image + 0x20, &function, context, &data, &frame, &ok));
    assert(ok && !data && get64(context, 0xf8) == stack[9]);

    /* At each prologue boundary only completed operations are undone. Even
     * at the end-of-prologue boundary there is no language handler to call.
     * No guest instruction execution or title-specific metadata is involved. */
    for (unsigned offset = 0; offset <= 5; ++offset) {
        memset(context, 0, sizeof(context));
        size_t rsp_index = offset < 1 ? 9 : offset < 4 ? 8 : 0;
        put64(context, 0x98, (uint64_t)(uintptr_t)&stack[rsp_index]);
        put64(context, 0x90, 0x7777);
        put64(context, 0xd8, 0x8888);
        assert(!beer_virtual_unwind(1, (uint64_t)(uintptr_t)image,
            (uint64_t)(uintptr_t)image + function.begin_address + offset,
            &function, context, &data, &frame, &ok));
        assert(ok && !data);
        assert(frame == (uint64_t)(uintptr_t)&stack[rsp_index]);
        assert(get64(context, 0x98) == (uint64_t)(uintptr_t)&stack[10]);
        assert(get64(context, 0xf8) == stack[9]);
        assert(get64(context, 0x90) == (offset >= 1 ? stack[8] : 0x7777));
        assert(get64(context, 0xd8) == (offset >= 5 ? stack[2] : 0x8888));
    }

    uint8_t before[sizeof(context)];
    memcpy(before, context, sizeof(context));
    unwind[0] = 2; /* unsupported version */
    assert(!beer_virtual_unwind(0, (uint64_t)(uintptr_t)image, 0, &function,
                                context, NULL, NULL, &ok));
    assert(!ok && !memcmp(before, context, sizeof(context)));

    BeerRuntimeFunction dynamic[] = {
        { 0x300, 0x340, 0x500 },
        { 0x100, 0x180, 0x400 }
    };
    uint64_t dynamic_base = 0x10000000;
    uint64_t lookup_base = 0;
    assert(beer_add_function_table(dynamic, 2, dynamic_base));
    assert(beer_lookup_dynamic_function_entry(dynamic_base + 0x120, &lookup_base) ==
           &dynamic[1]);
    assert(lookup_base == dynamic_base);
    assert(!beer_lookup_dynamic_function_entry(dynamic_base + 0x200, NULL));
    assert(beer_delete_function_table(dynamic));
    assert(!beer_delete_function_table(dynamic));
    assert(!beer_lookup_dynamic_function_entry(dynamic_base + 0x120, NULL));

    uint64_t callback_seen = 0;
    assert(beer_install_function_table_callback(0x20000003, 0x20000000, 0x1000,
                                                test_callback, &callback_seen));
    lookup_base = 0;
    assert(beer_lookup_dynamic_function_entry(0x20000050, &lookup_base) ==
           &callback_entry);
    assert(callback_seen == 0x20000050 && lookup_base == 0x20000000);
    assert(!beer_lookup_dynamic_function_entry(0x20001000, NULL));
    beer_reset_dynamic_function_tables();
    assert(!beer_lookup_dynamic_function_entry(0x20000050, NULL));

    puts("win32 unwind tests passed");
    return 0;
}
