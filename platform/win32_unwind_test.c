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

static const BeerRuntimeFunction *__attribute__((ms_abi))
test_callback(uint64_t control_pc, void *context)
{
    uint64_t *seen = context;
    *seen = control_pc;
    return &callback_entry;
}

int main(void)
{
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
    uint64_t handler = beer_virtual_unwind(0, (uint64_t)(uintptr_t)image,
                                            (uint64_t)(uintptr_t)image + 0x20,
                                            &function, context, &data, &frame, &ok);
    assert(ok);
    assert(handler == (uint64_t)(uintptr_t)image + 0x60);
    assert(data == (uint64_t)(uintptr_t)(unwind + 16));
    assert(get64(context, 0x90) == stack[8]);
    assert(get64(context, 0xd8) == stack[2]);
    assert(get64(context, 0xf8) == stack[9]);
    assert(get64(context, 0x98) == (uint64_t)(uintptr_t)&stack[10]);
    assert(frame == (uint64_t)(uintptr_t)&stack[9]);

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
