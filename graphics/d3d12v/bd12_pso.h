#ifndef BD12_PSO_H
#define BD12_PSO_H

#include "bd12.h"

/* Decoded D3D12 pipeline state description (legacy graphics/compute structs
 * and pipeline-state streams). Pointers reference guest memory and are only
 * valid for the duration of the creating call. */
typedef struct {
    uint32_t enable, logic_enable, src, dst, op, src_alpha, dst_alpha, op_alpha,
             logic_op, write_mask;
} BD12PsoBlend;

typedef struct {
    uint32_t fail, depth_fail, pass, func;
} BD12PsoStencil;

typedef struct {
    void *root_signature;                       /* guest COM pointer */
    const uint8_t *vs, *ps, *cs, *hs, *ds, *gs;
    size_t vs_size, ps_size, cs_size, hs_size, ds_size, gs_size;
    const uint8_t *input_elements;              /* D3D12_INPUT_ELEMENT_DESC[] */
    uint32_t input_count;
    uint32_t alpha_to_coverage, independent_blend;
    BD12PsoBlend blend[8];
    uint32_t sample_mask;
    uint32_t fill_mode, cull_mode, front_ccw;
    int32_t depth_bias;
    float depth_bias_clamp, slope_bias;
    uint32_t depth_clip;
    uint32_t depth_enable, depth_write_mask, depth_func, stencil_enable;
    uint8_t stencil_read_mask, stencil_write_mask;
    BD12PsoStencil stencil_front, stencil_back;
    uint32_t strip_cut, topology_type;
    uint32_t rt_count, rt_formats[8], ds_format;
    uint32_t samples;
    void *owned_blob;                           /* set by bd12_pso_clone */
} BD12PsoDesc;

void bd12_pso_defaults(BD12PsoDesc *pso);
/* D3D12_GRAPHICS_PIPELINE_STATE_DESC (656 bytes). */
void bd12_pso_parse_graphics(const uint8_t *desc, BD12PsoDesc *pso);
/* D3D12_COMPUTE_PIPELINE_STATE_DESC (56 bytes). */
void bd12_pso_parse_compute(const uint8_t *desc, BD12PsoDesc *pso);
/* D3D12_PIPELINE_STATE_STREAM_DESC payload. */
void bd12_pso_parse_stream(const uint8_t *stream, size_t size, BD12PsoDesc *pso);
/* Deep copy so a description outlives the creating call (lazy/async
 * compilation): shader bytes, input elements and semantic names are owned.
 * bd12_pso_release frees a clone; the clone's root_signature is not owned. */
int bd12_pso_clone(const BD12PsoDesc *src, BD12PsoDesc *dst);
void bd12_pso_release(BD12PsoDesc *pso);
/* NULL when translation or pipeline creation fails (reason on stderr). */
BD12GraphicsPipeline *bd12_pso_create_graphics(BD12Device *device,
                                               const BD12PsoDesc *pso);

/* D3D12_PRIMITIVE_TOPOLOGY (IASetPrimitiveTopology) -> Vulkan. */
VkPrimitiveTopology bd12_topology_to_vk(uint32_t d3d_topology,
                                        uint32_t *patch_control_points);

#endif
