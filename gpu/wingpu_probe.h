/* SPDX-License-Identifier: MIT */
#ifndef WG_PROBE_H
#define WG_PROBE_H
#include "wingpu_protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Shared by the actual Windows KMD and the host integration test. */
wg_u32 wg_encode_probe(void *batch, wg_u32 capacity, wg_u64 fence);
#ifdef __cplusplus
}
#endif
#endif
