#ifndef BD11_CONTEXT_STATE_H
#define BD11_CONTEXT_STATE_H

#include "bd11.h"

/* An owned state must be initialized before copy/reset/dispose. Capture writes
 * an uninitialized destination; copy replaces an initialized destination.
 * Sources must remain stable (hold the context guard when capturing live state).
 * Every binding owns one reference, even when several slots alias an object.
 * srv_res is borrowed; numeric fields are copied verbatim. */
void b11_pipeline_state_init(B11PipelineState *state);
void b11_pipeline_state_capture(B11PipelineState *dst, const B11PipelineState *src);
void b11_pipeline_state_copy(B11PipelineState *dst, const B11PipelineState *src);
void b11_pipeline_state_reset(B11PipelineState *state);
void b11_pipeline_state_dispose(B11PipelineState *state);
void b11_context_state_invalidate(B11Context *ctx);

/* Host-only mutable bank: not ID3DDeviceContextState, no emulated interface or
 * feature-level switching. Only the device's exact granted level is accepted.
 * st is saved on switch-out; the live context is authoritative while active.
 * Do not mutate/inspect st concurrently with a context using this bank. */
struct B11ContextState {
    _Atomic uint32_t refs;
    B11Device *dev;               /* retained */
    uint32_t feature_level;
    B11PipelineState st;
};
B11HR b11_context_state_create(B11Device *dev, uint32_t feature_level, B11ContextState **out);
void b11_context_state_retain(B11ContextState *state);
void b11_context_state_release(B11ContextState *state);
/* Immediate only, same-device only. NULL input does not touch state or output.
 * Errors leave state/output unchanged. Same-active input is a no-op and returns
 * a retained active bank if requested. Successful previous output is owned.
 * Uses the existing recursive context protection guard. */
B11HR b11_context_state_swap(B11Context *ctx, B11ContextState *state, B11ContextState **previous);

#endif