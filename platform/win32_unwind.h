#ifndef BEER_WIN32_UNWIND_H
#define BEER_WIN32_UNWIND_H

#include <stdint.h>

typedef struct BeerRuntimeFunction {
    uint32_t begin_address;
    uint32_t end_address;
    uint32_t unwind_data;
} BeerRuntimeFunction;

#if defined(__x86_64__)
typedef const BeerRuntimeFunction *(__attribute__((ms_abi)) *BeerFunctionTableCallback)(
    uint64_t control_pc, void *context);
#else
typedef const BeerRuntimeFunction *(*BeerFunctionTableCallback)(uint64_t control_pc,
                                                                 void *context);
#endif

/* Register runtime function metadata generated after image loading. These mirror
 * the Win64 RtlAdd/Delete/InstallFunctionTable APIs. The caller owns table and
 * callback storage until it unregisters the entry. */
int beer_add_function_table(const BeerRuntimeFunction *table, uint32_t count,
                            uint64_t base_address);
int beer_delete_function_table(const BeerRuntimeFunction *table);
int beer_install_function_table_callback(uint64_t table_identifier,
                                         uint64_t base_address,
                                         uint32_t length,
                                         BeerFunctionTableCallback callback,
                                         void *context);
const BeerRuntimeFunction *beer_lookup_dynamic_function_entry(uint64_t control_pc,
                                                               uint64_t *image_base);
void beer_reset_dynamic_function_tables(void);

/* Applies one AMD64 UNWIND_INFO record to a Windows CONTEXT. Returns the
 * language-specific exception handler, or zero when none is present. Invalid
 * or unsupported metadata leaves the context unchanged and sets *ok to zero. */
uint64_t beer_virtual_unwind(uint32_t handler_type, uint64_t image_base,
                            uint64_t control_pc,
                            const BeerRuntimeFunction *function_entry,
                            void *context_record, uint64_t *handler_data,
                            uint64_t *establisher_frame, int *ok);

#endif
