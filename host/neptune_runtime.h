/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_NEPTUNE_RUNTIME_H
#define WINIOS_NEPTUNE_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#include <sys/uio.h>
#include "fb_display.h"
#define HV_NEPTUNE_CAPSET 7u
typedef struct HvNeptune HvNeptune;
typedef struct HvNeptuneFence { uint32_t context, ring; uint64_t value; } HvNeptuneFence;
typedef struct HvNeptuneBlob {
    uint32_t resource, context, memory, flags;
    uint64_t blob_id, size;
    const struct iovec *iov;
    uint32_t iov_count;
} HvNeptuneBlob;
/* One runtime per process. All operations except completion callbacks are
 * serialized by the VM device thread. Completion callbacks never touch RAM. */
HvNeptune *hv_neptune_create(const char *native_dxmt_path,char *error,size_t error_size);
void hv_neptune_destroy(HvNeptune *runtime);
int hv_neptune_caps(HvNeptune *runtime,uint32_t *version,void *data,uint32_t *size);
int hv_neptune_context_create(HvNeptune *runtime,uint32_t id,uint32_t flags,const char *name,uint32_t length);
void hv_neptune_context_destroy(HvNeptune *runtime,uint32_t id);
int hv_neptune_submit(HvNeptune *runtime,uint32_t context,const void *commands,uint32_t bytes);
int hv_neptune_fence(HvNeptune *runtime,uint32_t context,uint32_t ring,uint64_t value);
int hv_neptune_pop_fence(HvNeptune *runtime,HvNeptuneFence *fence);
int hv_neptune_blob_create(HvNeptune *runtime,const HvNeptuneBlob *blob);
void hv_neptune_blob_unref(HvNeptune *runtime,uint32_t resource);
void hv_neptune_attach(HvNeptune *runtime,uint32_t context,uint32_t resource,int attach);
int hv_neptune_map_fixed(HvNeptune *runtime,uint32_t resource,void *address);
int hv_neptune_map_view(HvNeptune *runtime,uint32_t resource,void **address,uint64_t *length);
int hv_neptune_map_info(HvNeptune *runtime,uint32_t resource,uint32_t *info);
int hv_neptune_present(HvNeptune *runtime,HvFbDisplay *display,uint32_t resource,
    uint32_t width,uint32_t height,uint32_t stride,uint64_t offset,int rgba);
#endif
