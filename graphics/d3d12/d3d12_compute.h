/* d3d12_compute.h — removed. The CPU-emulation bypass paths that lived here
 * were never wired into the production dispatch path and violated the project
 * rule against title-specific shader-hash emulation. All compute workloads now
 * execute through the generic Vulkan path in d3d12_vulkan_compute.c.
 * This header is kept as an empty stub so any lingering include does not break
 * compilation; remove the include from callers when convenient. */
#ifndef BEER_D3D12_COMPUTE_H
#define BEER_D3D12_COMPUTE_H
#endif
