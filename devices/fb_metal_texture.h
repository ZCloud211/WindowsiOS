/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HV_FB_METAL_TEXTURE_H
#define HV_FB_METAL_TEXTURE_H
#import <Metal/Metal.h>
#include "fb_display.h"

/* Shared-memory scanout, following Henge's buffer_over_mapping/linear_texture_fits
 * design. The VM owns the pages; neither Metal object is allowed to free them.
 * Returns nil for unsupported layouts, leaving the upload path available. */
id<MTLTexture> hv_fb_metal_shared_texture(HvFbDisplay *fb, id<MTLDevice> device,
    const uint8_t *pixels, uint32_t w, uint32_t h, uint32_t stride,
    id<MTLBuffer> __strong *backing);
#endif
