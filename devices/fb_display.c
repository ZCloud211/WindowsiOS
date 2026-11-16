#include "fb_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

struct HvFbSurface { atomic_uint references; uint8_t *pixels; size_t length; };
struct HvFbNativeSurface {
    atomic_uint references;
    void *handle;
    uint32_t width, height;
    void (*release_handle)(void *);
};
HvFbNativeSurface *hv_fb_native_surface_create(void *handle,uint32_t w,uint32_t h,void (*release_handle)(void *)) {
    if(!handle || !release_handle || !w || !h || w>8192 || h>8192) return NULL;
    HvFbNativeSurface *s=calloc(1,sizeof *s);if(!s)return NULL;
    atomic_init(&s->references,1);s->handle=handle;s->width=w;s->height=h;s->release_handle=release_handle;return s;
}
void hv_fb_native_surface_retain(HvFbNativeSurface *s) {if(s)atomic_fetch_add_explicit(&s->references,1,memory_order_relaxed);}
void hv_fb_native_surface_release(HvFbNativeSurface *s) {
    if(s && atomic_fetch_sub_explicit(&s->references,1,memory_order_acq_rel)==1) {s->release_handle(s->handle);free(s);}
}
void *hv_fb_native_surface_handle(HvFbNativeSurface *s) {return s?s->handle:NULL;}
HvFbSurface *hv_fb_surface_create(size_t bytes) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || !bytes || bytes > (64u << 20)) return NULL;
    size_t length = (bytes + (size_t)page - 1) & ~((size_t)page - 1);
    HvFbSurface *surface = calloc(1, sizeof *surface);
    if (!surface) return NULL;
    if (posix_memalign((void **)&surface->pixels, (size_t)page, length)) { free(surface); return NULL; }
    memset(surface->pixels, 0, length);
    surface->length = length;
    atomic_init(&surface->references, 1);
    return surface;
}
uint8_t *hv_fb_surface_pixels(HvFbSurface *s) { return s ? s->pixels : NULL; }
void hv_fb_surface_retain(HvFbSurface *s) { if (s) atomic_fetch_add_explicit(&s->references, 1, memory_order_relaxed); }
void hv_fb_surface_release(HvFbSurface *s) {
    if (s && atomic_fetch_sub_explicit(&s->references, 1, memory_order_acq_rel) == 1) { free(s->pixels); free(s); }
}
void hv_fb_snapshot_release(HvFbSnapshot *frame) {
    hv_fb_surface_release(frame->surface);frame->surface=NULL;
    hv_fb_native_surface_release(frame->native_surface);frame->native_surface=NULL;
}
int hv_fb_display_acquire_frame(HvFbDisplay *fb, HvFbSnapshot *frame) {
    memset(frame, 0, sizeof *frame);
    if (!fb) return -1;
    pthread_mutex_lock(&fb->mu);
    int valid = fb->active && (fb->fb_host || fb->native_surface) && fb->width && fb->height;
    if (valid) {
        *frame = (HvFbSnapshot){.pixels=fb->fb_host,.width=fb->width,.height=fb->height,.stride=fb->stride,
            .surface=fb->surface,.revision=fb->revision,.native_surface=fb->native_surface};
        hv_fb_surface_retain(frame->surface);
        hv_fb_native_surface_retain(frame->native_surface);
    }
    pthread_mutex_unlock(&fb->mu);
    return valid ? 0 : -1;
}
int hv_fb_display_set_surface(HvFbDisplay *fb, HvFbSurface *surface, size_t offset,
                              uint32_t w, uint32_t h, uint32_t stride) {
    if (!fb || !surface || !w || !h || w > 8192 || h > 8192 || stride < (uint64_t)w * 4 ||
        stride > 65536 || offset >= surface->length || (uint64_t)stride * (h-1) + (uint64_t)w*4 > surface->length - offset) return -1;
    hv_fb_surface_retain(surface);
    pthread_mutex_lock(&fb->mu);
    HvFbSurface *old = fb->surface;
    HvFbNativeSurface *old_native=fb->native_surface;fb->native_surface=NULL;
    fb->surface = surface; fb->fb_host = surface->pixels + offset; fb->fb_pa = 0;
    fb->revision++;
    fb->width = w; fb->height = h; fb->stride = stride; fb->fourcc = 0x34325258; fb->active = 1;
    pthread_mutex_unlock(&fb->mu);
    hv_fb_surface_release(old);
    hv_fb_native_surface_release(old_native);
    return 0;
}
int hv_fb_display_set_native_surface(HvFbDisplay *fb,HvFbNativeSurface *surface) {
    if(!fb || !surface)return -1;
    hv_fb_native_surface_retain(surface);
    pthread_mutex_lock(&fb->mu);
    HvFbSurface *old=fb->surface;HvFbNativeSurface *old_native=fb->native_surface;
    fb->surface=NULL;fb->native_surface=surface;fb->fb_host=NULL;fb->fb_pa=0;
    fb->width=surface->width;fb->height=surface->height;fb->stride=0;
    fb->active=1;fb->revision++;
    pthread_mutex_unlock(&fb->mu);
    hv_fb_surface_release(old);hv_fb_native_surface_release(old_native);return 0;
}
void hv_fb_display_restore_firmware(HvFbDisplay *fb) {
    if (!fb) return;
    pthread_mutex_lock(&fb->mu);
    HvFbSurface *old = fb->surface;
    HvFbNativeSurface *old_native=fb->native_surface;fb->native_surface=NULL;
    fb->surface = NULL; fb->fb_pa = fb->firmware_pa; fb->fb_host = fb->firmware_host;
    fb->revision++;
    fb->width = fb->firmware_w; fb->height = fb->firmware_h;
    fb->stride = fb->firmware_stride; fb->fourcc = fb->firmware_fourcc;
    fb->active = fb->fb_host != NULL;
    pthread_mutex_unlock(&fb->mu);
    hv_fb_surface_release(old);
    hv_fb_native_surface_release(old_native);
}
void hv_fb_display_dirty(HvFbDisplay *fb) {
    pthread_mutex_lock(&fb->mu); fb->revision++; pthread_mutex_unlock(&fb->mu);
}

/* RAMFBCfg written by EDK2 QemuRamFbDxe via fw_cfg DMA to "etc/ramfb" (28 bytes, big-endian):
   struct RAMFBCfg {
       uint64_t addr;
       uint32_t fourcc;
       uint32_t flags;
       uint32_t width;
       uint32_t height;
       uint32_t stride;
   } __attribute__((packed));
*/

static uint32_t be32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static uint64_t be64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

static void on_ramfb_write(void *opaque, const uint8_t *data, uint32_t len) {
    HvFbDisplay *fb = opaque;
    if (len < 28) return;

    uint64_t addr = be64(data + 0);
    uint32_t fourcc = be32(data + 8);
    uint32_t width = be32(data + 16);
    uint32_t height = be32(data + 20);
    uint32_t stride = be32(data + 24);

    if (width < 64 || width > 8192 || height < 64 || height > 8192) return;
    if (stride == 0) stride = width * 4;
    if (stride < width * 2 || stride > 65536) return;

    uint64_t total_bytes = (uint64_t)stride * (height - 1) + (uint64_t)width * 4;
    void *host_ptr = hv_phys_ptr(fb->m, addr, total_bytes);
    if (!host_ptr) {
        fprintf(stderr, "winvm: ramfb ignored invalid PA 0x%llx (%ux%u stride %u)\n",
                (unsigned long long)addr, width, height, stride);
        return;
    }

    pthread_mutex_lock(&fb->mu);
    fb->firmware_pa = addr; fb->firmware_host = host_ptr;
    fb->firmware_w = width; fb->firmware_h = height;
    fb->firmware_stride = stride; fb->firmware_fourcc = fourcc;
    if (fb->surface || fb->native_surface) { pthread_mutex_unlock(&fb->mu); return; }
    fb->fb_pa = addr;
    fb->fb_host = (const uint8_t *)host_ptr;
    fb->width = width;
    fb->height = height;
    fb->stride = stride;
    fb->fourcc = fourcc;
    fb->active = 1;
    pthread_mutex_unlock(&fb->mu);

    fprintf(stderr, "winvm: ramfb configured %ux%u stride=%u fourcc=0x%08x at PA 0x%llx\n",
            width, height, stride, fourcc, (unsigned long long)addr);
}

HvFbDisplay *hv_fb_display_create(HvMachine *m, HvFwCfg *fw_cfg) {
    HvFbDisplay *fb = calloc(1, sizeof *fb);
    fb->m = m;
    pthread_mutex_init(&fb->mu, NULL);
    if (fw_cfg) {
        hv_fw_cfg_add_writable_file(fw_cfg, "etc/ramfb", 28, on_ramfb_write, fb);
    }
    return fb;
}

int hv_fb_display_get_size(HvFbDisplay *fb,uint32_t *w,uint32_t *h) {
    if(!fb)return -1;
    pthread_mutex_lock(&fb->mu);
    int valid=fb->active && fb->width && fb->height;
    if(valid) {if(w)*w=fb->width;if(h)*h=fb->height;}
    pthread_mutex_unlock(&fb->mu);return valid?0:-1;
}
int hv_fb_display_get_frame(HvFbDisplay *fb, const uint8_t **pixels, uint32_t *w, uint32_t *h, uint32_t *stride) {
    if (!fb) return -1;
    pthread_mutex_lock(&fb->mu);
    if (!fb->active || !fb->fb_host || !fb->width || !fb->height) {
        pthread_mutex_unlock(&fb->mu);
        return -1;
    }
    *pixels = fb->fb_host;
    *w = fb->width;
    *h = fb->height;
    *stride = fb->stride;
    pthread_mutex_unlock(&fb->mu);
    return 0;
}

int hv_fb_display_shared_range(HvFbDisplay *fb, const uint8_t *pixels, uint32_t w, uint32_t h,
                               uint32_t stride, size_t page_size, HvFbSharedRange *out) {
    memset(out, 0, sizeof *out);
    if (!fb || !pixels || !page_size || (page_size & (page_size - 1)) || !w || !h ||
        w > 8192 || h > 8192 || stride < (uint64_t)w * 4 || stride > 65536) return -1;
    pthread_mutex_lock(&fb->mu);
    int rc = -1;
    if (!fb->active || fb->fb_host != pixels || fb->width != w || fb->height != h || fb->stride != stride) goto done;
    uintptr_t address = (uintptr_t)pixels;
    size_t offset = address & (page_size - 1);
    uint64_t bytes = (uint64_t)stride * h;
    if (bytes > SIZE_MAX - offset - (page_size - 1)) goto done;
    size_t length = ((size_t)bytes + offset + page_size - 1) & ~(page_size - 1);
    void *base = (void *)(address - offset);
    if (fb->surface) {
        uintptr_t start = (uintptr_t)fb->surface->pixels;
        if ((uintptr_t)base < start || (uintptr_t)base - start > fb->surface->length ||
            length > fb->surface->length - ((uintptr_t)base - start)) goto done;
    } else if (fb->fb_pa < offset || hv_phys_ptr(fb->m, fb->fb_pa - offset, length) != base) goto done;
    *out = (HvFbSharedRange){base, length, offset, fb->surface};
    rc = 0;
done:
    pthread_mutex_unlock(&fb->mu);
    return rc;
}
