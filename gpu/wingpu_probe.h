/* SPDX-License-Identifier: MIT */
#ifndef WG_PROBE_H
#define WG_PROBE_H
#include "wingpu_protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Shared by the actual Windows KMD and the host integration test. */
uint32_t wg_encode_probe(void *batch, uint32_t capacity, uint64_t fence);
#ifdef __cplusplus
}
#endif
#endif
