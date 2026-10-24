#include "win32_seh.h"

#include <stddef.h>
#include <string.h>

#define CONTEXT_RSP_OFFSET 0x98u
#define CONTEXT_RIP_OFFSET 0xf8u
#define CONTEXT_AMD64_SIZE 0x4d0u
#define UNW_FLAG_EHANDLER 1u

static uint64_t context_u64(const void *context, size_t offset)
{
    uint64_t value;
    memcpy(&value, (const uint8_t *)context + offset, sizeof(value));
    return value;
}

static void context_put_u64(void *context, size_t offset, uint64_t value)
{
    memcpy((uint8_t *)context + offset, &value, sizeof(value));
}

static int stack_contains(uint64_t low, uint64_t high, uint64_t address,
                          uint64_t size)
{
    return low && high > low && address >= low && size <= high - address;
}

int beer_seh_dispatch(BeerExceptionRecord64 *record, void *context_record,
                      BeerSehLookupFunction lookup,
                      BeerSehAddressValidator executable_address,
                      void *opaque, uint64_t stack_low, uint64_t stack_high,
                      uint32_t max_frames, uint32_t *visited_frames)
{
    if (visited_frames) *visited_frames = 0;
    if (!record || !context_record || !lookup || !max_frames ||
        stack_high <= stack_low)
        return 0;
    if (record->flags & BEER_EXCEPTION_NONCONTINUABLE)
        return 0;

    uint8_t walk_context[CONTEXT_AMD64_SIZE] __attribute__((aligned(16)));
    memcpy(walk_context, context_record, sizeof(walk_context));

    for (uint32_t frame_index = 0; frame_index < max_frames; ++frame_index) {
        uint64_t control_pc = context_u64(walk_context, CONTEXT_RIP_OFFSET);
        uint64_t old_rsp = context_u64(walk_context, CONTEXT_RSP_OFFSET);
        if (!control_pc || !stack_contains(stack_low, stack_high, old_rsp, 8))
            return 0;

        uint64_t image_base = 0;
        const BeerRuntimeFunction *entry = lookup(control_pc, &image_base, opaque);
        if (!entry) {
            uint64_t caller;
            memcpy(&caller, (const void *)(uintptr_t)old_rsp, sizeof(caller));
            if (!caller || (executable_address &&
                            !executable_address(caller, opaque)))
                return 0;
            context_put_u64(walk_context, CONTEXT_RSP_OFFSET, old_rsp + 8);
            context_put_u64(walk_context, CONTEXT_RIP_OFFSET, caller);
            if (visited_frames) *visited_frames = frame_index + 1;
            continue;
        }

        uint64_t handler_data = 0;
        uint64_t establisher_frame = 0;
        int unwind_ok = 0;
        uint64_t handler = beer_virtual_unwind(UNW_FLAG_EHANDLER, image_base,
                                                control_pc, entry, walk_context,
                                                &handler_data,
                                                &establisher_frame, &unwind_ok);
        uint64_t new_rsp = context_u64(walk_context, CONTEXT_RSP_OFFSET);
        if (!unwind_ok || new_rsp <= old_rsp || new_rsp > stack_high ||
            establisher_frame < stack_low || establisher_frame >= stack_high)
            return 0;
        if (visited_frames) *visited_frames = frame_index + 1;

        if (handler) {
            if (executable_address && !executable_address(handler, opaque))
                return 0;
            BeerDispatcherContext64 dispatcher;
            memset(&dispatcher, 0, sizeof(dispatcher));
            dispatcher.control_pc = control_pc;
            dispatcher.image_base = image_base;
            dispatcher.function_entry = entry;
            dispatcher.establisher_frame = establisher_frame;
            dispatcher.context_record = walk_context;
            dispatcher.language_handler = handler;
            dispatcher.handler_data = handler_data;
            dispatcher.scope_index = 0;

            int32_t disposition = ((BeerLanguageHandler)(uintptr_t)handler)(
                record, establisher_frame, context_record, &dispatcher);
            if (disposition == BEER_EXCEPTION_DISPOSITION_CONTINUE_EXECUTION)
                return 1;
            if (disposition != BEER_EXCEPTION_DISPOSITION_CONTINUE_SEARCH)
                return 0;
        }
    }
    return 0;
}
