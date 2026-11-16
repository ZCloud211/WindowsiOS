/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HV_WINGPU_METAL_H
#define HV_WINGPU_METAL_H
#include <stddef.h>
#include <stdint.h>
#include "../gpu/wingpu_protocol.h"
typedef struct HvWinGpuMetal HvWinGpuMetal;
typedef void (*HvWinGpuDone)(void *context, uint32_t error, uint64_t gpu_ns);
HvWinGpuMetal *hv_wingpu_metal_create(void);
/* Caller serializes submissions. All descriptors are validated before encoding.
 * Success schedules one asynchronous completion; rejection schedules none. */
uint32_t hv_wingpu_metal_submit(HvWinGpuMetal *gpu, const WgCommand *commands,
                                uint32_t count, HvWinGpuDone done, void *context);
void hv_wingpu_metal_destroy(HvWinGpuMetal *gpu);
/* Host-test readback only, never part of the display/present path. */
int hv_wingpu_metal_test_readback(HvWinGpuMetal *gpu, uint32_t resource, void *bgra, size_t bytes);
#endif
