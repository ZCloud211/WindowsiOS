/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HV_GPU_APERTURE_H
#define HV_GPU_APERTURE_H
#include "machine.h"
typedef struct HvGpuAperture HvGpuAperture;
/* Guest 4KB pages may alias independently mapped 16KB host allocations. */
HvGpuAperture *hv_gpu_aperture_create(HvMachine *m,uint64_t size);
void hv_gpu_aperture_relocate(HvGpuAperture *a,uint64_t pa);
/* Takes ownership of mapping on success; munmap after all CPUs forget it. */
int hv_gpu_aperture_map(HvGpuAperture *a,uint32_t resource,uint64_t offset,void *mapping,uint64_t length);
int hv_gpu_aperture_unmap(HvGpuAperture *a,uint32_t resource);
void hv_gpu_aperture_poll(HvGpuAperture *a);
int hv_gpu_aperture_ready(HvGpuAperture *a);
/* Only after CPUs have stopped. The RAM metadata belongs to HvMachine. */
void hv_gpu_aperture_destroy(HvGpuAperture *a);
#endif
