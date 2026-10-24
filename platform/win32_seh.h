#ifndef BEER_WIN32_SEH_H
#define BEER_WIN32_SEH_H

#include <stdint.h>

#include "win32_unwind.h"

#define BEER_EXCEPTION_NONCONTINUABLE 0x1u
#define BEER_EXCEPTION_UNWINDING      0x2u
#define BEER_EXCEPTION_EXIT_UNWIND    0x4u
#define BEER_EXCEPTION_STACK_INVALID  0x8u
#define BEER_EXCEPTION_NESTED_CALL    0x10u
#define BEER_EXCEPTION_TARGET_UNWIND  0x20u
#define BEER_EXCEPTION_COLLIDED_UNWIND 0x40u

#define BEER_EXCEPTION_DISPOSITION_CONTINUE_EXECUTION 0
#define BEER_EXCEPTION_DISPOSITION_CONTINUE_SEARCH    1
#define BEER_EXCEPTION_DISPOSITION_NESTED_EXCEPTION   2
#define BEER_EXCEPTION_DISPOSITION_COLLIDED_UNWIND    3

typedef struct BeerExceptionRecord64 {
    uint32_t code;
    uint32_t flags;
    uint64_t nested_record;
    uint64_t address;
    uint32_t parameter_count;
    uint32_t unused_alignment;
    uint64_t information[15];
} BeerExceptionRecord64;

typedef struct BeerDispatcherContext64 {
    uint64_t control_pc;
    uint64_t image_base;
    const BeerRuntimeFunction *function_entry;
    uint64_t establisher_frame;
    uint64_t target_ip;
    void *context_record;
    uint64_t language_handler;
    uint64_t handler_data;
    void *history_table;
    uint32_t scope_index;
    uint32_t fill0;
} BeerDispatcherContext64;

typedef const BeerRuntimeFunction *(*BeerSehLookupFunction)(uint64_t control_pc,
                                                            uint64_t *image_base,
                                                            void *opaque);
typedef int (*BeerSehAddressValidator)(uint64_t address, void *opaque);

typedef int32_t (__attribute__((ms_abi)) *BeerLanguageHandler)(
    BeerExceptionRecord64 *record, uint64_t establisher_frame,
    void *context_record, BeerDispatcherContext64 *dispatcher_context);

/* Performs first-pass AMD64 frame-based exception dispatch. Vectored handlers
 * must run before this function. Returns one only when a language handler asks
 * to continue execution. Malformed metadata, invalid stack frames, unsupported
 * dispositions, and an exhausted frame walk safely return zero. */
int beer_seh_dispatch(BeerExceptionRecord64 *record, void *context_record,
                      BeerSehLookupFunction lookup,
                      BeerSehAddressValidator executable_address,
                      void *opaque, uint64_t stack_low, uint64_t stack_high,
                      uint32_t max_frames, uint32_t *visited_frames);

#endif
