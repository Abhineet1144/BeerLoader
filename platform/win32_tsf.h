#ifndef BEER_WIN32_TSF_H
#define BEER_WIN32_TSF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns nonzero when CLSID_TF_ThreadMgr was recognized.  In that case
 * result receives the COM HRESULT and output follows normal COM ownership. */
int beer_tsf_create_instance(const uint8_t *class_id, const uint8_t *interface_id,
                             void *outer, void **output, uint64_t *result);

#ifdef __cplusplus
}
#endif

#endif
