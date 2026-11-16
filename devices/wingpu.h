/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HV_WINGPU_H
#define HV_WINGPU_H
#include "pcie.h"
#include "wingpu_metal.h"
typedef struct HvWinGpu HvWinGpu;
typedef struct HvWinGpuStats {
    uint64_t submitted, completed, failed, last_fence, gpu_nanoseconds;
    uint32_t enabled, busy, last_error;
} HvWinGpuStats;
/* Experimental Windows driver bring-up device; opt-in only, no Direct3D. */
HvWinGpu *hv_wingpu_create(HvMachine *m, HvPcie *bus);
void hv_wingpu_poll(HvWinGpu *gpu);
/* Read-only snapshot: does not pump completions or access guest memory. */
void hv_wingpu_stats(HvWinGpu *gpu, HvWinGpuStats *stats);
void hv_wingpu_destroy(HvWinGpu *gpu);
HvPciDev *hv_wingpu_pci(HvWinGpu *gpu);
int hv_wingpu_test_readback(HvWinGpu *gpu, uint32_t resource, void *bgra, size_t bytes);
#endif
