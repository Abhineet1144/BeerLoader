#include "win32_seh.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t image[512];
static BeerRuntimeFunction functions[2];

static void put64(void *context, unsigned offset, uint64_t value)
{
    memcpy((uint8_t *)context + offset, &value, sizeof(value));
}

static uint64_t get64(const void *context, unsigned offset)
{
    uint64_t value;
    memcpy(&value, (const uint8_t *)context + offset, sizeof(value));
    return value;
}

static const BeerRuntimeFunction *lookup(uint64_t pc, uint64_t *base, void *opaque)
{
    (void)opaque;
    uint64_t relative = pc - (uint64_t)(uintptr_t)image;
    for (unsigned i = 0; i < 2; ++i) {
        if (relative >= functions[i].begin_address &&
            relative < functions[i].end_address) {
            *base = (uint64_t)(uintptr_t)image;
            return &functions[i];
        }
    }
    return NULL;
}

static int executable(uint64_t address, void *opaque)
{
    (void)opaque;
    return address >= (uint64_t)(uintptr_t)image &&
           address < (uint64_t)(uintptr_t)image + sizeof(image);
}

static int32_t __attribute__((ms_abi, section(".beer_test_handler"))) continue_handler(
    BeerExceptionRecord64 *record, uint64_t frame, void *context,
    BeerDispatcherContext64 *dispatcher)
{
    uint64_t *state = (uint64_t *)(uintptr_t)record->information[14];
    assert(record->code == 0xc0000005u);
    assert(frame == state[0]);
    assert(dispatcher->establisher_frame == frame);
    assert(dispatcher->handler_data == state[1]);
    put64(context, 0xf8, (uint64_t)(uintptr_t)image + 0x1f0);
    state[2]++;
    return BEER_EXCEPTION_DISPOSITION_CONTINUE_EXECUTION;
}

static void setup_unwind(unsigned offset, uint64_t handler)
{
    uint8_t *info = image + offset;
    info[0] = 1 | (1 << 3); /* version 1, EHANDLER */
    info[1] = 0;
    info[2] = 0;
    info[3] = 0;
    *(uint32_t *)(info + 4) = (uint32_t)(handler - (uint64_t)(uintptr_t)image);
    *(uint32_t *)(info + 8) = 0x12345678u;
}

int main(void)
{
    memset(image, 0, sizeof(image));
    uint64_t base = (uint64_t)(uintptr_t)image;
    uint64_t host_handler = (uint64_t)(uintptr_t)continue_handler;
    assert(host_handler > base && host_handler - base <= UINT32_MAX);
    functions[0] = (BeerRuntimeFunction){ 0x20, 0x40, 0x100 };
    functions[1] = (BeerRuntimeFunction){ 0x80, 0xa0, 0x108 };
    setup_unwind(0x108, host_handler);

    /* The first metadata frame has no handler; its caller is a leaf frame. The
     * leaf return reaches the second metadata frame, whose handler resumes. */
    image[0x100] = 1;
    uint64_t stack[16] = {0};
    uint8_t context[0x4d0] = {0};
    put64(context, 0xf8, base + 0x30);
    put64(context, 0x98, (uint64_t)(uintptr_t)&stack[0]);
    stack[0] = base + 0x60; /* leaf frame */
    stack[1] = base + 0x90; /* frame with EHANDLER */
    stack[2] = base + 0xc0;
    uint64_t state[3] = {
        (uint64_t)(uintptr_t)&stack[2],
        (uint64_t)(uintptr_t)(image + 0x110),
        0
    };

    BeerExceptionRecord64 record = {0};
    record.code = 0xc0000005u;
    record.information[14] = (uint64_t)(uintptr_t)state;
    uint32_t visited = 0;
    assert(beer_seh_dispatch(&record, context, lookup, NULL, NULL,
                             (uint64_t)(uintptr_t)&stack[0],
                             (uint64_t)(uintptr_t)&stack[16], 8, &visited));
    assert(state[2] == 1);
    assert(visited == 3);
    assert(get64(context, 0xf8) == base + 0x1f0);

    state[2] = 0;
    put64(context, 0xf8, base + 0x30);
    put64(context, 0x98, (uint64_t)(uintptr_t)&stack[0]);
    assert(!beer_seh_dispatch(&record, context, lookup, executable, NULL,
                              (uint64_t)(uintptr_t)&stack[0],
                              (uint64_t)(uintptr_t)&stack[16], 8, NULL));
    assert(state[2] == 0); /* host test handler rejected as non-guest code */

    puts("win32 SEH tests passed");
    return 0;
}
