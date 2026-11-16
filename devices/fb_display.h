#ifndef WINVM_FB_DISPLAY_H
#define WINVM_FB_DISPLAY_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_cfg.h"
#include "machine.h"
#include "pcie.h"

typedef struct HvFbSurface HvFbSurface;
HvFbSurface *hv_fb_surface_create(size_t bytes);
void hv_fb_surface_retain(HvFbSurface *surface);
void hv_fb_surface_release(HvFbSurface *surface);
uint8_t *hv_fb_surface_pixels(HvFbSurface *surface);

/* A retained native GPU image. No CPU pixel mapping or readback is required.
 * create takes ownership of handle on success; release may run on any thread. */
typedef struct HvFbNativeSurface HvFbNativeSurface;
HvFbNativeSurface *hv_fb_native_surface_create(void *handle, uint32_t width, uint32_t height,
                                               void (*release_handle)(void *));
void hv_fb_native_surface_retain(HvFbNativeSurface *surface);
void hv_fb_native_surface_release(HvFbNativeSurface *surface);
void *hv_fb_native_surface_handle(HvFbNativeSurface *surface);

/* Linear framebuffer display supporting QEMU `ramfb` (etc/ramfb in fw_cfg, used by
   EDK2 ArmVirtQemu GOP -> winload.efi -> Windows 11 BasicDisplay.sys + DWM)
   as well as optional VBE/bochs scanout. Zero MMIO exits per pixel write! */

typedef struct HvFbDisplay {
    HvMachine *m;
    pthread_mutex_t mu;

    /* Active scanout geometry (protected by mu) */
    uint64_t fb_pa;
    const uint8_t *fb_host;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t fourcc; /* DRM fourcc, e.g. XR24 / AR24 (32-bit BGRX) */
    int active;
    uint64_t revision;
    HvFbSurface *surface;
    HvFbNativeSurface *native_surface;
    uint64_t firmware_pa;
    const uint8_t *firmware_host;
    uint32_t firmware_w, firmware_h, firmware_stride, firmware_fourcc;

    /* Metal presenter handle (created lazily by fb_metal.m) */
    void *metal;
} HvFbDisplay;

HvFbDisplay *hv_fb_display_create(HvMachine *m, HvFwCfg *fw_cfg);
/* Thread-safe snapshot of current scanout pointer and dimensions; returns 0 if active, -1 if not yet set. */
int hv_fb_display_get_frame(HvFbDisplay *fb, const uint8_t **pixels, uint32_t *w, uint32_t *h, uint32_t *stride);
int hv_fb_display_get_size(HvFbDisplay *fb, uint32_t *w, uint32_t *h);
typedef struct HvFbSnapshot {
    const uint8_t *pixels;
    uint32_t width, height, stride;
    HvFbSurface *surface;
    uint64_t revision;
    HvFbNativeSurface *native_surface;
} HvFbSnapshot;
int hv_fb_display_acquire_frame(HvFbDisplay *fb, HvFbSnapshot *frame);
void hv_fb_snapshot_release(HvFbSnapshot *frame);
int hv_fb_display_set_surface(HvFbDisplay *fb, HvFbSurface *surface, size_t offset,
                              uint32_t width, uint32_t height, uint32_t stride);
int hv_fb_display_set_native_surface(HvFbDisplay *fb, HvFbNativeSurface *surface);
void hv_fb_display_restore_firmware(HvFbDisplay *fb);
void hv_fb_display_dirty(HvFbDisplay *fb);

/* Page-aligned, fully validated guest RAM backing for a shared Metal buffer.
 * Refuse a stale frame or any range that would include unmapped guest memory. */
typedef struct HvFbSharedRange {
    void *base;
    size_t length;
    size_t offset;
    HvFbSurface *surface; /* Borrowed; caller must hold a frame snapshot. */
} HvFbSharedRange;
int hv_fb_display_shared_range(HvFbDisplay *fb, const uint8_t *pixels, uint32_t w, uint32_t h,
                               uint32_t stride, size_t page_size, HvFbSharedRange *out);

typedef struct HvFbMetalStats {
    char device[128];
    char error[256];
    int ready;
    int shared_scanout;
    int native_scanout;
    uint64_t completed;
    uint64_t failed;
} HvFbMetalStats;

/* Metal layer presentation & PNG screenshot helpers (implemented in devices/fb_metal.m) */
void hv_fb_metal_set_layer(HvFbDisplay *fb, void *ca_metal_layer);
int hv_fb_metal_set_texture(HvFbDisplay *fb, void *metal_texture);
HvFbNativeSurface *hv_fb_metal_surface_from_shared_fd(int fd, uint64_t length,
    uint32_t width, uint32_t height, uint32_t stride, uint64_t offset, int rgba);
uint64_t hv_fb_metal_frames(HvFbDisplay *fb);
void hv_fb_metal_stats(HvFbDisplay *fb, HvFbMetalStats *stats);
int hv_fb_save_png(HvFbDisplay *fb, const char *path);

#endif
