#include "d3d12_shader_translator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) return 0;
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    *data = malloc((size_t)length);
    if (!*data || fread(*data, 1, (size_t)length, file) != (size_t)length) {
        free(*data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *size = (size_t)length;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s shader.dxbc\n", argv[0]);
        return 2;
    }
    uint8_t *dxbc = NULL;
    size_t dxbc_size = 0;
    if (!read_file(argv[1], &dxbc, &dxbc_size)) return 3;

    const BeerD3D12TranslatedShader *translated =
        beer_d3d12_translate_shader(dxbc, dxbc_size,
                                    UINT64_C(0x31b662b443572be7));
    if (!translated) {
        fprintf(stderr, "%s\n", beer_d3d12_shader_translation_error());
        free(dxbc);
        return 4;
    }
    if (translated->spirv_size < 20 ||
        memcmp(translated->spirv, "\x03\x02\x23\x07", 4) != 0)
        return 5;

    int found_srv = 0, found_uav = 0;
    uint32_t srv_binding = UINT32_MAX, uav_binding = UINT32_MAX;
    for (uint32_t i = 0; i < translated->descriptor_count; ++i) {
        const BeerD3D12ShaderDescriptor *descriptor =
            &translated->descriptors[i];
        if (descriptor->set != 0)
            return 6;
        if (descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_SRV &&
            descriptor->register_index == 0 && descriptor->register_space == 0) {
            found_srv = 1;
            srv_binding = descriptor->binding;
        }
        if (descriptor->type == BEER_D3D12_SHADER_DESCRIPTOR_UAV &&
            descriptor->register_index == 0 && descriptor->register_space == 0) {
            found_uav = 1;
            uav_binding = descriptor->binding;
        }
    }
    /* Bindings are derived from descriptor class plus register, not scan
     * order. Buffer and image SRV/UAV classes are separate so stage-local D3D
     * t0 declarations cannot collide in a merged graphics pipeline. */
    if (!found_srv || !found_uav || srv_binding != 0 ||
        uav_binding != (2u << 13) || srv_binding == uav_binding)
        return 7;

    const BeerD3D12TranslatedShader *cached =
        beer_d3d12_translate_shader(dxbc, dxbc_size,
                                    UINT64_C(0x31b662b443572be7));
    if (cached != translated) return 8;

    static const uint8_t invalid[] = {'N', 'O', 'P', 'E'};
    if (beer_d3d12_translate_shader(invalid, sizeof(invalid), 1) != NULL)
        return 9;

    free(dxbc);
    beer_d3d12_shader_translator_destroy();
    puts("D3D12 automatic shader translation tests passed");
    return 0;
}
