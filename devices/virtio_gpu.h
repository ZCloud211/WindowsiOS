/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HV_VIRTIO_GPU_H
#define HV_VIRTIO_GPU_H
#include "virtio.h"
#include "fb_display.h"
typedef struct HvVirtioGpu HvVirtioGpu;
typedef struct HvVirtioGpuStats {
    uint64_t commands, transfers, transferred_bytes, flushes, errors;
    uint64_t submissions_3d, blob_creates, blob_maps, retired_fences;
    uint32_t contexts_3d, pending_fences;
    uint8_t backend_3d_ready;
    uint32_t width, height;
    uint8_t driver_status, msix_enabled;
    uint16_t config_vector, control_vector, cursor_vector;
    uint16_t control_enabled, cursor_enabled;
} HvVirtioGpuStats;
HvVirtioGpu *hv_virtio_gpu_create(HvMachine *machine, HvPcie *bus, HvFbDisplay *display);
HvVirtio *hv_virtio_gpu_device(HvVirtioGpu *gpu);
void hv_virtio_gpu_stats(HvVirtioGpu *gpu, HvVirtioGpuStats *stats);
/* Caller holds machine device lock, as for notify. */
void hv_virtio_gpu_poll(HvVirtioGpu *gpu);
void hv_virtio_gpu_destroy(HvVirtioGpu *gpu);
#endif
