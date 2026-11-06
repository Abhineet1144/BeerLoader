/* ID3D12PipelineLibrary blob and name logic (no GPU): build with -DBD12_PIPELIB_STANDALONE. */
#include "bd12_pipelib.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint16_t *wide(const char *text)
{
    static uint16_t buffers[8][64];
    static int next;
    uint16_t *out = buffers[next++ % 8];
    size_t i = 0;
    for (; text[i]; ++i) out[i] = (uint8_t)text[i];
    out[i] = 0;
    return out;
}

int main(void)
{
    uint8_t identity[BD12_PIPELIB_IDENTITY_SIZE];
    for (int i = 0; i < BD12_PIPELIB_IDENTITY_SIZE; ++i) identity[i] = (uint8_t)(i * 7 + 1);

    /* An empty blob is an empty library; names are unique. */
    Bd12Pipelib lib;
    bd12_pipelib_init(&lib, identity);
    assert(bd12_pipelib_load(&lib, NULL, 0) == 0 && lib.count == 0);
    assert(bd12_pipelib_add(&lib, wide("ps_main_0001"), 0) == 0);
    assert(bd12_pipelib_add(&lib, wide("cs_blur"), 1) == 0);
    assert(bd12_pipelib_add(&lib, wide("cs_blur"), 1) == -1);
    assert(bd12_pipelib_find(&lib, wide("cs_blur")) == 1);
    assert(bd12_pipelib_find(&lib, wide("cs_blu")) == -1);
    assert(bd12_pipelib_find(&lib, NULL) == -1);

    /* Serialize and load back on the same GPU and driver. */
    const size_t size = bd12_pipelib_serialized_size(&lib);
    uint8_t *blob = malloc(size);
    assert(!bd12_pipelib_serialize(&lib, blob, size - 1));
    assert(bd12_pipelib_serialize(&lib, blob, size));
    Bd12Pipelib again;
    bd12_pipelib_init(&again, identity);
    assert(bd12_pipelib_load(&again, blob, size) == 0 && again.count == 2);
    assert(bd12_pipelib_find(&again, wide("ps_main_0001")) == 0 && again.entries[0].kind == 0);
    assert(bd12_pipelib_find(&again, wide("cs_blur")) == 1 && again.entries[1].kind == 1);
    bd12_pipelib_clear(&again);

    /* Another GPU, another driver, another implementation's blob, a damaged blob. */
    uint8_t other[BD12_PIPELIB_IDENTITY_SIZE];
    memcpy(other, identity, sizeof(other));
    other[4] ^= 1;                                       /* deviceID */
    bd12_pipelib_init(&again, other);
    assert(bd12_pipelib_load(&again, blob, size) == BD12_PIPELIB_HR_ADAPTER_NOT_FOUND && again.count == 0);
    memcpy(other, identity, sizeof(other));
    other[8] ^= 1;                                       /* driverVersion */
    bd12_pipelib_init(&again, other);
    assert(bd12_pipelib_load(&again, blob, size) == BD12_PIPELIB_HR_DRIVER_VERSION_MISMATCH);
    memcpy(other, identity, sizeof(other));
    other[20] ^= 1;                                      /* pipelineCacheUUID */
    bd12_pipelib_init(&again, other);
    assert(bd12_pipelib_load(&again, blob, size) == BD12_PIPELIB_HR_DRIVER_VERSION_MISMATCH);
    bd12_pipelib_init(&again, identity);
    static const uint8_t foreign[64] = { 'V', 'K', 'D', '3', 'D' };
    assert(bd12_pipelib_load(&again, foreign, sizeof(foreign)) == BD12_PIPELIB_HR_DRIVER_VERSION_MISMATCH);
    assert(bd12_pipelib_load(&again, blob, size - 1) == UINT64_C(0x80070057) && again.count == 0);
    blob[size - 1] ^= 0;                                  /* intact again */
    uint8_t *longer = malloc(size + 2);
    memcpy(longer, blob, size);
    longer[size] = longer[size + 1] = 0;
    assert(bd12_pipelib_load(&again, longer, size + 2) == UINT64_C(0x80070057) && again.count == 0);

    free(longer);
    free(blob);
    bd12_pipelib_clear(&lib);
    puts("pipeline library tests passed");
    return 0;
}
