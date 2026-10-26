/* Offline probe: runs dxil-spirv over captured DXIL libraries (BEER_DXR_DUMP output) and reports, per export, the
 * shader stage, every resource it touches and whether conversion succeeds.
 * Build: gcc -O1 -Ithird_party/dxil-spirv -o /tmp/dxr_probe tools/dxr_probe.c -ldl
 * Run:   /tmp/dxr_probe third_party/dxil-spirv/build/libdxil-spirv-c-shared.so lib_*.dxil */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dxil_spirv_c.h"

static void *L;
#define SYM(name) static __typeof__(&dxil_spv_##name) p_##name
SYM(parse_dxil_blob); SYM(parsed_blob_free); SYM(parsed_blob_get_num_entry_points); SYM(parsed_blob_get_entry_point_name);
SYM(parsed_blob_get_shader_stage_for_entry); SYM(create_converter); SYM(converter_free); SYM(converter_set_entry_point);
SYM(converter_set_srv_remapper); SYM(converter_set_sampler_remapper); SYM(converter_set_uav_remapper); SYM(converter_set_cbv_remapper);
SYM(converter_run); SYM(converter_get_compiled_spirv); SYM(begin_thread_allocator_context); SYM(end_thread_allocator_context);
SYM(converter_add_option); SYM(set_thread_log_callback); SYM(parsed_blob_get_num_rdat_subobjects);

typedef struct { int srv, uav, cbv, smp, as, fail; char note[600]; } Seen;
static const char *kind_name(unsigned k)
{
    static const char *n[] = {"?","1D","2D","2DMS","3D","Cube","1DArr","2DArr","2DMSArr","CubeArr","TypedBuf","RawBuf","StructBuf","CBV","Sampler","TBuf","AS"};
    return k < 17 ? n[k] : "?";
}
static void note(Seen *s, const char *c, const dxil_spv_d3d_binding *d)
{
    char t[80];
    snprintf(t, sizeof(t), " %s:%s(s%u,r%u,n%u)", c, kind_name(d->kind), d->register_space, d->register_index, d->range_size);
    if (strlen(s->note) + strlen(t) < sizeof(s->note)) strcat(s->note, t);
}
static dxil_spv_bool srv(void *u, const dxil_spv_d3d_binding *d, dxil_spv_srv_vulkan_binding *o)
{ memset(o, 0, sizeof(*o)); ((Seen *)u)->srv++; if (d->kind == 16) ((Seen *)u)->as++; note(u, "SRV", d); o->buffer_binding.binding = 1; return DXIL_SPV_TRUE; }
static dxil_spv_bool smp(void *u, const dxil_spv_d3d_binding *d, dxil_spv_vulkan_binding *o)
{ memset(o, 0, sizeof(*o)); ((Seen *)u)->smp++; note(u, "SMP", d); o->binding = 2; return DXIL_SPV_TRUE; }
static dxil_spv_bool uav(void *u, const dxil_spv_uav_d3d_binding *d, dxil_spv_uav_vulkan_binding *o)
{ memset(o, 0, sizeof(*o)); ((Seen *)u)->uav++; note(u, "UAV", &d->d3d_binding); o->buffer_binding.binding = 3; return DXIL_SPV_TRUE; }
static dxil_spv_bool cbv(void *u, const dxil_spv_d3d_binding *d, dxil_spv_cbv_vulkan_binding *o)
{ memset(o, 0, sizeof(*o)); ((Seen *)u)->cbv++; note(u, "CBV", d); o->vulkan.uniform_binding.binding = 4; o->vulkan.uniform_binding.descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_UBO; return DXIL_SPV_TRUE; }
static void logger(void *u, dxil_spv_log_level l, const char *m) { (void)u; if (l >= DXIL_SPV_LOG_LEVEL_WARN) fprintf(stderr, "    [dxil] %s", m); }

int main(int argc, char **argv)
{
    L = dlopen(argv[1], RTLD_NOW);
    if (!L) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
#define LD(name) p_##name = (void *)dlsym(L, "dxil_spv_" #name); if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 1; }
    LD(parse_dxil_blob) LD(parsed_blob_free) LD(parsed_blob_get_num_entry_points) LD(parsed_blob_get_entry_point_name)
    LD(parsed_blob_get_shader_stage_for_entry) LD(create_converter) LD(converter_free) LD(converter_set_entry_point)
    LD(converter_set_srv_remapper) LD(converter_set_sampler_remapper) LD(converter_set_uav_remapper) LD(converter_set_cbv_remapper)
    LD(converter_run) LD(converter_get_compiled_spirv) LD(begin_thread_allocator_context) LD(end_thread_allocator_context)
    LD(converter_add_option) LD(set_thread_log_callback) LD(parsed_blob_get_num_rdat_subobjects)
    int total = 0, failed = 0;
    for (int a = 2; a < argc; ++a) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
        void *data = malloc(size);
        if (fread(data, 1, size, f) != (size_t)size) { fclose(f); free(data); continue; }
        fclose(f);
        p_begin_thread_allocator_context();
        p_set_thread_log_callback(logger, NULL);
        dxil_spv_parsed_blob blob;
        if (p_parse_dxil_blob(data, size, &blob) != DXIL_SPV_SUCCESS) { printf("%s: parse failed\n", argv[a]); p_end_thread_allocator_context(); free(data); continue; }
        unsigned n = 0;
        p_parsed_blob_get_num_entry_points(blob, &n);
        printf("%s: %u entry points, %u rdat subobjects\n", argv[a], n, p_parsed_blob_get_num_rdat_subobjects(blob));
        for (unsigned i = 0; i < n; ++i) {
            const char *name = NULL;
            p_parsed_blob_get_entry_point_name(blob, i, &name);
            dxil_spv_shader_stage st = p_parsed_blob_get_shader_stage_for_entry(blob, name);
            dxil_spv_converter c;
            Seen seen;
            memset(&seen, 0, sizeof(seen));
            p_create_converter(blob, &c);
            p_converter_set_entry_point(c, name);
            p_converter_set_srv_remapper(c, srv, &seen);
            p_converter_set_sampler_remapper(c, smp, &seen);
            p_converter_set_uav_remapper(c, uav, &seen);
            p_converter_set_cbv_remapper(c, cbv, &seen);
            dxil_spv_result r = p_converter_run(c);
            dxil_spv_compiled_spirv sp = {0};
            if (r == DXIL_SPV_SUCCESS) p_converter_get_compiled_spirv(c, &sp);
            ++total; if (r != DXIL_SPV_SUCCESS) ++failed;
            printf("  %-24s stage=%2d result=%d spirv=%zu words srv=%d uav=%d cbv=%d smp=%d as=%d%s\n", name, st, r, sp.size / 4, seen.srv, seen.uav, seen.cbv, seen.smp, seen.as, seen.note);
            p_converter_free(c);
        }
        p_parsed_blob_free(blob);
        p_end_thread_allocator_context();
        free(data);
    }
    printf("converted %d entry points, %d failed\n", total, failed);
    return 0;
}
