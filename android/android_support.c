/* Small bionic gaps the loader relies on (Android NDK, API < 33). */
#include <unwind.h>
#include "beer_arch.h"

#if __ANDROID_API__ < 33
struct backtrace_state { void **buffer; int size; int count; };

static _Unwind_Reason_Code backtrace_step(struct _Unwind_Context *context, void *opaque)
{
    struct backtrace_state *state = opaque;
    uintptr_t pc = _Unwind_GetIP(context);
    if (pc) {
        if (state->count >= state->size) return _URC_END_OF_STACK;
        state->buffer[state->count++] = (void *)pc;
    }
    return _URC_NO_REASON;
}

int backtrace(void **buffer, int size)
{
    struct backtrace_state state = { buffer, size, 0 };
    _Unwind_Backtrace(backtrace_step, &state);
    return state.count;
}
#endif
