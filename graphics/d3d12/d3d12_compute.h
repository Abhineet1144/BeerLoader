#ifndef BEER_D3D12_COMPUTE_H
#define BEER_D3D12_COMPUTE_H

#include "d3d12_compat.h"

/* Executes compute workloads whose semantics have been validated against their
 * captured shader bytecode. Unknown shaders are rejected explicitly. */
int beer_d3d12_execute_known_compute(
    const BeerD3D12ComputeDispatch *dispatch);

#endif
