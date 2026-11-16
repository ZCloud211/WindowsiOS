#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <TargetConditionals.h>

#include <os/lock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "fb_display.h"
#include "fb_metal_texture.h"

static void release_native_texture(void *handle) { CFRelease(handle); }
HvFbNativeSurface *hv_fb_metal_surface_from_shared_fd(int fd,uint64_t length,uint32_t w,uint32_t h,
    uint32_t stride,uint64_t offset,int rgba) {
    @autoreleasepool {
    long page=sysconf(_SC_PAGESIZE);
    if(fd<0 || page<=0 || !length || length>SIZE_MAX || !w || !h || w>8192 || h>8192 ||
       stride<(uint64_t)w*4 || offset>length || (uint64_t)(h-1)*stride+(uint64_t)w*4>length-offset) return NULL;
    size_t mapped_length=((size_t)length+(size_t)page-1)&~((size_t)page-1);
    struct stat st;
    if(mapped_length<(size_t)length || fstat(fd,&st) || st.st_size<0 || (uint64_t)st.st_size<length) return NULL;
    /* This is an anonymous shared resource, not a guest disk. Extend its last
       4KB page to the host page boundary for Metal's no-copy buffer. */
    if((uint64_t)st.st_size<mapped_length && ftruncate(fd,(off_t)mapped_length))return NULL;
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return NULL;
    MTLPixelFormat format=rgba?MTLPixelFormatRGBA8Unorm:MTLPixelFormatBGRA8Unorm;
    NSUInteger alignment=[device minimumLinearTextureAlignmentForPixelFormat:format];
    if(!alignment || offset%alignment || stride%alignment || mapped_length>device.maxBufferLength) return NULL;
    void *mapped=mmap(NULL,mapped_length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(mapped==MAP_FAILED)return NULL;
    id<MTLBuffer> buffer=[device newBufferWithBytesNoCopy:mapped length:mapped_length options:MTLResourceStorageModeShared
        deallocator:^(void *ptr,NSUInteger bytes) {munmap(ptr,bytes);}];
    if(!buffer) {munmap(mapped,mapped_length);return NULL;}
    MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
    desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> image=[buffer newTextureWithDescriptor:desc offset:(NSUInteger)offset bytesPerRow:stride];
    if(!image)return NULL;
    void *retained=(__bridge_retained void *)image;
    HvFbNativeSurface *surface=hv_fb_native_surface_create(retained,w,h,release_native_texture);
    if(!surface)CFRelease(retained);
    return surface;
    }
}
int hv_fb_metal_set_texture(HvFbDisplay *fb,void *metal_texture) {
    id<MTLTexture> image=(__bridge id<MTLTexture>)metal_texture;
    if(!fb || !image || image.textureType!=MTLTextureType2D || image.sampleCount!=1 ||
       !(image.usage&MTLTextureUsageShaderRead)) return -1;
    switch(image.pixelFormat) {
        case MTLPixelFormatBGRA8Unorm:case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatRGBA8Unorm:case MTLPixelFormatRGBA8Unorm_sRGB:break;
        default:return -1;
    }
    void *retained=(__bridge_retained void *)image;
    HvFbNativeSurface *surface=hv_fb_native_surface_create(retained,(uint32_t)image.width,(uint32_t)image.height,release_native_texture);
    if(!surface) {CFRelease(retained);return -1;}
    int rc=hv_fb_display_set_native_surface(fb,surface);
    hv_fb_native_surface_release(surface);return rc;
}

id<MTLTexture> hv_fb_metal_shared_texture(HvFbDisplay *fb, id<MTLDevice> device,
    const uint8_t *pixels, uint32_t w, uint32_t h, uint32_t stride,
    id<MTLBuffer> __strong *backing) {
    *backing = nil;
    if (!device || !device.hasUnifiedMemory) return nil;
    HvFbSharedRange range;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || hv_fb_display_shared_range(fb, pixels, w, h, stride, (size_t)page_size, &range)) return nil;
    NSUInteger alignment = [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm];
    if (!alignment || range.offset % alignment || stride % alignment || range.length > device.maxBufferLength) return nil;
    HvFbSurface *owner = range.surface;
    hv_fb_surface_retain(owner);
    id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:range.base length:range.length
        options:MTLResourceStorageModeShared deallocator:^(void *pointer, NSUInteger length) {
            (void)pointer; (void)length; hv_fb_surface_release(owner);
        }];
    if (!buffer) { hv_fb_surface_release(owner); return nil; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> result = [buffer newTextureWithDescriptor:td offset:range.offset bytesPerRow:stride];
    if (result) *backing = buffer;
    return result;
}

static NSString *const kFbShaderSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VOut {\n"
     "    float4 pos [[position]];\n"
     "    float2 uv;\n"
     "};\n"
     "vertex VOut fb_vs(uint vid [[vertex_id]], constant float2 &scale [[buffer(0)]]) {\n"
     "    const float2 quad[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };\n"
     "    const float2 uvs[4]  = { float2(0,1),   float2(1,1),  float2(0,0),  float2(1,0) };\n"
     "    VOut o;\n"
     "    o.pos = float4(quad[vid] * scale, 0.0, 1.0);\n"
     "    o.uv  = uvs[vid];\n"
     "    return o;\n"
     "}\n"
     "fragment float4 fb_fs(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {\n"
     "    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);\n"
     "    float4 c = tex.sample(s, in.uv);\n"
     "    return float4(c.rgb, 1.0);\n"
     "}\n";

@interface HvFbMetalPresenter : NSObject {
@public
    HvFbDisplay *fb;
    os_unfair_lock lock;
    CAMetalLayer *layer;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> pipeline;
    id<MTLTexture> texture;
    id<MTLBuffer> sharedBacking;
    const uint8_t *texturePixels;
    uint32_t texW, texH, texStride;
    dispatch_source_t timer;
    _Atomic(uint64_t) framesPresented;
    _Atomic(uint64_t) framesCompleted;
    _Atomic(uint64_t) framesFailed;
    _Atomic(uint64_t) inFlight;
    NSString *lastError;
    uint64_t lastHash;
    CGSize lastDrawableSize;
}
- (instancetype)initWithDisplay:(HvFbDisplay *)display;
- (void)setTargetLayer:(CAMetalLayer *)newLayer;
- (void)tick;
@end

@implementation HvFbMetalPresenter

- (instancetype)initWithDisplay:(HvFbDisplay *)display {
    if ((self = [super init])) {
        fb = display;
        lock = OS_UNFAIR_LOCK_INIT;
        device = MTLCreateSystemDefaultDevice();
        if (!device) lastError = @"Metal device unavailable";
        if (device) {
            queue = [device newCommandQueue];
            NSError *err = nil;
            id<MTLLibrary> lib = [device newLibraryWithSource:kFbShaderSource options:nil error:&err];
            if (lib) {
                MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
                pd.vertexFunction = [lib newFunctionWithName:@"fb_vs"];
                pd.fragmentFunction = [lib newFunctionWithName:@"fb_fs"];
                pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&err];
            }
            if (!queue || !pipeline) lastError = err.localizedDescription ?: @"Metal queue or pipeline unavailable";
        }
    }
    return self;
}

- (void)setTargetLayer:(CAMetalLayer *)newLayer {
    os_unfair_lock_lock(&lock);
    layer = newLayer;
    lastHash = 0;
    lastDrawableSize = CGSizeZero;
    if (layer && device) {
        dispatch_block_t cfg = ^{
            newLayer.device = self->device;
            newLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            newLayer.framebufferOnly = YES;
        };
        if ([NSThread isMainThread]) cfg();
        else dispatch_async(dispatch_get_main_queue(), cfg);

        if (!timer) {
            dispatch_queue_t q = dispatch_queue_create("me.ammaar.windowsios.fbmetal", DISPATCH_QUEUE_SERIAL);
            timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
            dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, NSEC_PER_SEC / 60, 2 * NSEC_PER_MSEC);
            __weak HvFbMetalPresenter *weakSelf = self;
            dispatch_source_set_event_handler(timer, ^{
                [weakSelf tick];
            });
            dispatch_resume(timer);
        }
    } else if (!layer && timer) {
        dispatch_source_cancel(timer);
        timer = nil;
    }
    os_unfair_lock_unlock(&lock);
}

static uint64_t quick_frame_hash(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride) {
    uint64_t hash = ((uint64_t)w << 32) ^ h ^ stride;
    uint32_t step_y = h > 128 ? h / 128 : 1;
    for (uint32_t y = 0; y < h; y += step_y) {
        const uint64_t *row = (const uint64_t *)(px + (size_t)y * stride);
        uint32_t qwords = (w * 4) / 8;
        uint32_t step_x = qwords > 64 ? qwords / 64 : 1;
        for (uint32_t x = 0; x < qwords; x += step_x) {
            hash = (hash ^ row[x]) * 0x9e3779b97f4a7c15ull;
        }
    }
    return hash ? hash : 1;
}

- (void)tick {
    @autoreleasepool {
        HvFbSnapshot frame __attribute__((cleanup(hv_fb_snapshot_release))) = {0};
        if (hv_fb_display_acquire_frame(fb, &frame)) return;
        const uint8_t *px = frame.pixels;
        uint32_t w = frame.width, h = frame.height, stride = frame.stride;
        id<MTLTexture> nativeImage=(__bridge id<MTLTexture>)hv_fb_native_surface_handle(frame.native_surface);

        os_unfair_lock_lock(&lock);
        CAMetalLayer *curLayer = layer;
        if (!curLayer || !device || !pipeline || !queue) {
            os_unfair_lock_unlock(&lock);
            return;
        }

        CGSize dsize = curLayer.drawableSize;
        if (dsize.width < 4 || dsize.height < 4) {
            os_unfair_lock_unlock(&lock);
            return;
        }

        /* VirtIO flushes include small cursor/text updates missed by pixel sampling. */
        uint64_t hval = (frame.surface || nativeImage) ? frame.revision : quick_frame_hash(px, w, h, stride);
        BOOL sizeChanged = !CGSizeEqualToSize(dsize, lastDrawableSize);
        if (atomic_load_explicit(&inFlight, memory_order_relaxed) >= 3 ||
            (hval == lastHash && !sizeChanged && texW == w && texH == h && texStride == stride && texturePixels == px)) {
            os_unfair_lock_unlock(&lock);
            return;
        }

        if(nativeImage) {
            texture=nativeImage;sharedBacking=nil;
            texW=w;texH=h;texStride=0;texturePixels=NULL;
        } else if (!texture || texW != w || texH != h || texStride != stride || texturePixels != px) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:w
                                                                                         height:h
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            texture = hv_fb_metal_shared_texture(fb, device, px, w, h, stride, &sharedBacking);
            if (!texture) texture = [device newTextureWithDescriptor:td];
            texW = w;
            texH = h;
            texStride = stride;
            texturePixels = px;
        }

        if (!texture) {
            lastError = @"Metal framebuffer texture allocation failed";
            os_unfair_lock_unlock(&lock);
            return;
        }
        if (!nativeImage && !sharedBacking) [texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
                   mipmapLevel:0
                     withBytes:px
                   bytesPerRow:stride];

        id<CAMetalDrawable> drawable = [curLayer nextDrawable];
        if (!drawable) {
            os_unfair_lock_unlock(&lock);
            return;
        }

        float dstAspect = (float)(dsize.width / dsize.height);
        float srcAspect = (float)w / (float)h;
        float scale[2] = {1.0f, 1.0f};
        if (dstAspect > srcAspect) {
            scale[0] = srcAspect / dstAspect;
        } else {
            scale[1] = dstAspect / srcAspect;
        }

        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (!cb || !enc) {
            lastError = @"Metal command encoder unavailable";
            os_unfair_lock_unlock(&lock);
            return;
        }
        [enc setRenderPipelineState:pipeline];
        [enc setVertexBytes:scale length:sizeof(scale) atIndex:0];
        [enc setFragmentTexture:texture atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];
        __weak HvFbMetalPresenter *weakSelf = self;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            HvFbMetalPresenter *p = weakSelf;
            if (!p) return;
            if (done.status == MTLCommandBufferStatusCompleted) {
                atomic_fetch_add_explicit(&p->framesCompleted, 1, memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(&p->framesFailed, 1, memory_order_relaxed);
                os_unfair_lock_lock(&p->lock);
                p->lastError = done.error.localizedDescription ?: @"Metal command buffer failed";
                p->lastHash = 0; /* Retry the unchanged frame after an actual GPU failure. */
                os_unfair_lock_unlock(&p->lock);
            }
            atomic_fetch_sub_explicit(&p->inFlight, 1, memory_order_relaxed);
        }];
        [cb presentDrawable:drawable];
        atomic_fetch_add_explicit(&inFlight, 1, memory_order_relaxed);
        [cb commit];

        lastHash = hval;
        lastDrawableSize = dsize;
        atomic_fetch_add_explicit(&framesPresented, 1, memory_order_relaxed);
        os_unfair_lock_unlock(&lock);
    }
}

@end

static HvFbMetalPresenter *ensure_presenter(HvFbDisplay *fb) {
    if (!fb) return nil;
    pthread_mutex_lock(&fb->mu);
    HvFbMetalPresenter *p = (__bridge HvFbMetalPresenter *)fb->metal;
    if (!p) {
        p = [[HvFbMetalPresenter alloc] initWithDisplay:fb];
        fb->metal = (__bridge_retained void *)p;
    }
    pthread_mutex_unlock(&fb->mu);
    return p;
}

void hv_fb_metal_set_layer(HvFbDisplay *fb, void *ca_metal_layer) {
    HvFbMetalPresenter *p = ensure_presenter(fb);
    if (!p) return;
    [p setTargetLayer:(__bridge CAMetalLayer *)ca_metal_layer];
}

uint64_t hv_fb_metal_frames(HvFbDisplay *fb) {
    if (!fb || !fb->metal) return 0;
    HvFbMetalPresenter *p = (__bridge HvFbMetalPresenter *)fb->metal;
    return atomic_load_explicit(&p->framesPresented, memory_order_relaxed);
}

void hv_fb_metal_stats(HvFbDisplay *fb, HvFbMetalStats *stats) {
    memset(stats, 0, sizeof *stats);
    if (!fb) return;
    pthread_mutex_lock(&fb->mu);
    HvFbMetalPresenter *p = (__bridge HvFbMetalPresenter *)fb->metal;
    pthread_mutex_unlock(&fb->mu);
    if (!p) return;
    os_unfair_lock_lock(&p->lock);
    snprintf(stats->device, sizeof stats->device, "%s", p->device.name.UTF8String ?: "");
    snprintf(stats->error, sizeof stats->error, "%s", p->lastError.UTF8String ?: "");
    stats->ready = p->device && p->queue && p->pipeline;
    stats->shared_scanout = p->sharedBacking != nil;
    stats->native_scanout = p->texture != nil && p->texturePixels == NULL;
    stats->completed = atomic_load_explicit(&p->framesCompleted, memory_order_relaxed);
    stats->failed = atomic_load_explicit(&p->framesFailed, memory_order_relaxed);
    os_unfair_lock_unlock(&p->lock);
}

int hv_fb_save_png(HvFbDisplay *fb, const char *path) {
    if (!fb || !path) return -1;
    HvFbSnapshot frame __attribute__((cleanup(hv_fb_snapshot_release))) = {0};
    if (hv_fb_display_acquire_frame(fb, &frame)) return -1;
    const uint8_t *px = frame.pixels;
    uint32_t w = frame.width, h = frame.height, stride = frame.stride;
    if(!px) return -1; /* Native GPU images have no CPU mapping. */

    @autoreleasepool {
        size_t total = (size_t)stride * h;
        uint8_t *copy = malloc(total);
        if (!copy) return -1;
        memcpy(copy, px, total);
        /* Ensure alpha byte is 0xff (XRGB8888 leaves X=0x00) */
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = copy + (size_t)y * stride;
            for (uint32_t x = 0; x < w; x++) row[x * 4 + 3] = 0xff;
        }

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef prov = CGDataProviderCreateWithData(NULL, copy, total, NULL);
        CGBitmapInfo bmi = kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst;
        CGImageRef img = CGImageCreate(w, h, 8, 32, stride, cs, bmi, prov, NULL, false, kCGRenderingIntentDefault);
        int rc = -1;
        if (img) {
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
            CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL);
            if (dst) {
                CGImageDestinationAddImage(dst, img, NULL);
                if (CGImageDestinationFinalize(dst)) rc = 0;
                CFRelease(dst);
            }
            CGImageRelease(img);
        }
        CGDataProviderRelease(prov);
        CGColorSpaceRelease(cs);
        free(copy);
        return rc;
    }
}
