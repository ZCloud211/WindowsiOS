/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "fb_metal_texture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Mac-only GPU verification, never launches or connects to the iPad. */
int main(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device || !device.hasUnifiedMemory) {
            fprintf(stderr, "A local Apple Silicon Metal device is required; GPU verification was not run.\n");
            return 77;
        }
        const uint32_t w = 800, h = 257, stride = w * 4;
        const size_t page = (size_t)sysconf(_SC_PAGESIZE);
        const size_t size = ((stride * h + 64 + page - 1) & ~(page - 1)) + page;
        uint8_t *ram = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        assert(ram != MAP_FAILED);
        HvMachine *machine = calloc(1, sizeof *machine);
        hv_machine_add_ram(machine, 0x40000000, size, ram, 0);
        HvFbDisplay *fb = hv_fb_display_create(machine, NULL);
        fb->active = 1;
        fb->fb_pa = 0x40000000 + page + 64;
        fb->fb_host = ram + page + 64;
        fb->width = w; fb->height = h; fb->stride = stride;
        HvFbSharedRange range;
        assert(!hv_fb_display_shared_range(fb, fb->fb_host, w, h, stride, page, &range));
        assert(range.base == ram + page && range.offset == 64 && range.length == size - page);
        assert(hv_fb_display_shared_range(fb, fb->fb_host + 4, w, h, stride, page, &range));
        assert(hv_fb_display_shared_range(fb, fb->fb_host, w, h + 1, stride, page, &range));
        assert(hv_fb_display_shared_range(fb, fb->fb_host, w, h, stride, 123, &range));

        id<MTLBuffer> backing = nil;
        id<MTLTexture> texture = hv_fb_metal_shared_texture(fb, device, fb->fb_host, w, h, stride, &backing);
        assert(texture && backing && backing.contents == ram + page);
        assert(texture.storageMode == MTLStorageModeShared && texture.buffer == backing);
        NSError *error = nil;
        NSString *shader = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void check_pixels(texture2d<float, access::read> image [[texture(0)]],"
            "device uchar4 *out [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {"
            "if (p.x < image.get_width() && p.y < image.get_height())"
            "out[p.y * image.get_width() + p.x] = uchar4(round(image.read(p) * 255.0)); }";
        id<MTLLibrary> library = [device newLibraryWithSource:shader options:nil error:&error];
        assert(library);
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"check_pixels"] error:&error];
        assert(pipeline);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> result = [device newBufferWithLength:(size_t)w * h * 4 options:MTLResourceStorageModeShared];
        assert(queue && result);
        uint8_t *pixels = (uint8_t *)fb->fb_host;
        for (int pass = 0; pass < 2; pass++) {
            for (size_t n = 0; n < (size_t)w * h; n++) {
                pixels[n * 4] = (uint8_t)(17 + pass);
                pixels[n * 4 + 1] = 34;
                pixels[n * 4 + 2] = (uint8_t)(51 + pass);
                pixels[n * 4 + 3] = 255;
            }
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
            [encoder setTexture:texture atIndex:0];
            [encoder setBuffer:result offset:0 atIndex:0];
            [encoder dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 8, 1)];
            [encoder endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) {
                fprintf(stderr, "Metal verification failed: %s\n", cb.error.localizedDescription.UTF8String);
                return 1;
            }
            const uint8_t *out = result.contents;
            for (size_t n = 0; n < (size_t)w * h; n++) {
                assert(out[n * 4] == 51 + pass && out[n * 4 + 1] == 34 &&
                       out[n * 4 + 2] == 17 + pass && out[n * 4 + 3] == 255);
            }
        }
        texture = nil; backing = nil;
        fb->fb_host += 4; fb->fb_pa += 4;
        assert(!hv_fb_metal_shared_texture(fb, device, fb->fb_host, w, h, stride, &backing));
        assert(!backing); /* The caller must use its upload fallback. */
        fb->fb_pa = 0x40000000 + size - 4;
        assert(hv_fb_display_shared_range(fb, fb->fb_host, w, h, stride, page, &range));
        /* A real VirtIO-owned surface remains alive after mode change/unref
         * until Metal releases its no-copy buffer, even without a live scanout. */
        HvFbSurface *surface = hv_fb_surface_create((size_t)stride * h);
        assert(surface);
        memset(hv_fb_surface_pixels(surface), 0x6b, (size_t)stride * h);
        assert(!hv_fb_display_set_surface(fb, surface, 0, w, h, stride));
        HvFbSnapshot frame;
        assert(!hv_fb_display_acquire_frame(fb, &frame));
        texture = hv_fb_metal_shared_texture(fb, device, frame.pixels, w, h, stride, &backing);
        assert(texture && backing && texture.buffer == backing);
        hv_fb_surface_release(surface);
        hv_fb_display_restore_firmware(fb);
        hv_fb_snapshot_release(&frame);
        id<MTLCommandBuffer> ownedCB = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ownedEncoder = [ownedCB computeCommandEncoder];
        [ownedEncoder setComputePipelineState:pipeline];
        [ownedEncoder setTexture:texture atIndex:0];
        [ownedEncoder setBuffer:result offset:0 atIndex:0];
        [ownedEncoder dispatchThreads:MTLSizeMake(w,h,1) threadsPerThreadgroup:MTLSizeMake(16,8,1)];
        [ownedEncoder endEncoding]; [ownedCB commit]; [ownedCB waitUntilCompleted];
        assert(ownedCB.status == MTLCommandBufferStatusCompleted);
        const uint8_t *ownedPixels = result.contents;
        for (size_t n = 0; n < (size_t)w*h*4; n++) assert(ownedPixels[n] == 0x6b);
        texture = nil; backing = nil;
        puts("PASS: Metal reads VirtIO-owned scanout after guest mode change/unref; no-copy buffer owns the allocation");
        MTLTextureDescriptor *nativeDesc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
        nativeDesc.storageMode=MTLStorageModePrivate;
        nativeDesc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        id<MTLTexture> nativeImage=[device newTextureWithDescriptor:nativeDesc];assert(nativeImage);
        MTLRenderPassDescriptor *clear=[MTLRenderPassDescriptor renderPassDescriptor];
        clear.colorAttachments[0].texture=nativeImage;
        clear.colorAttachments[0].loadAction=MTLLoadActionClear;clear.colorAttachments[0].storeAction=MTLStoreActionStore;
        clear.colorAttachments[0].clearColor=MTLClearColorMake(1,0,0,1);
        id<MTLCommandBuffer> nativeCB=[queue commandBuffer];id<MTLRenderCommandEncoder> draw=[nativeCB renderCommandEncoderWithDescriptor:clear];
        [draw endEncoding];[nativeCB commit];[nativeCB waitUntilCompleted];assert(nativeCB.status==MTLCommandBufferStatusCompleted);
        assert(!hv_fb_metal_set_texture(fb,(__bridge void *)nativeImage));
        HvFbSnapshot nativeFrame;assert(!hv_fb_display_acquire_frame(fb,&nativeFrame));
        uint32_t nativeW=0,nativeH=0;assert(!hv_fb_display_get_size(fb,&nativeW,&nativeH) && nativeW==w && nativeH==h);
        assert(!nativeFrame.pixels && !nativeFrame.surface && nativeFrame.native_surface);
        texture=(__bridge id<MTLTexture>)hv_fb_native_surface_handle(nativeFrame.native_surface);
        assert(texture==nativeImage && texture.storageMode==MTLStorageModePrivate);
        nativeImage=nil;clear=nil;hv_fb_display_restore_firmware(fb);
        nativeCB=[queue commandBuffer];id<MTLComputeCommandEncoder> sample=[nativeCB computeCommandEncoder];
        [sample setComputePipelineState:pipeline];[sample setTexture:texture atIndex:0];[sample setBuffer:result offset:0 atIndex:0];
        [sample dispatchThreads:MTLSizeMake(w,h,1) threadsPerThreadgroup:MTLSizeMake(16,8,1)];[sample endEncoding];[nativeCB commit];[nativeCB waitUntilCompleted];
        assert(nativeCB.status==MTLCommandBufferStatusCompleted);
        const uint8_t *nativePixels=result.contents;
        for(size_t n=0;n<(size_t)w*h;n++) assert(nativePixels[n*4]==255 && nativePixels[n*4+1]==0 && nativePixels[n*4+2]==0 && nativePixels[n*4+3]==255);
        texture=nil;hv_fb_snapshot_release(&nativeFrame);
        puts("PASS: GPU-private native texture scanout, no CPU framebuffer, retained through mode change and sampled correctly on Metal");
        /* SHM exported by Neptune can be 4KB sized on a 16KB Apple host. */
        char sharedPath[]="/private/tmp/winios-metal-shm-XXXXXX";
        int sharedFD=mkstemp(sharedPath);assert(sharedFD>=0);unlink(sharedPath);
        size_t sharedBytes=((size_t)stride*h+4095)&~(size_t)4095;
        assert(!ftruncate(sharedFD,(off_t)sharedBytes));
        uint8_t *producer=mmap(NULL,sharedBytes,PROT_READ|PROT_WRITE,MAP_SHARED,sharedFD,0);assert(producer!=MAP_FAILED);
        HvFbNativeSurface *sharedSurface=hv_fb_metal_surface_from_shared_fd(sharedFD,sharedBytes,w,h,stride,0,0);
        assert(sharedSurface);close(sharedFD);
        assert(!hv_fb_display_set_native_surface(fb,sharedSurface));hv_fb_native_surface_release(sharedSurface);
        HvFbSnapshot sharedFrame;assert(!hv_fb_display_acquire_frame(fb,&sharedFrame));
        texture=(__bridge id<MTLTexture>)hv_fb_native_surface_handle(sharedFrame.native_surface);
        for(int pass=0;pass<2;pass++) {
            for(size_t n=0;n<(size_t)w*h;n++) {producer[4*n]=21+pass;producer[4*n+1]=42;producer[4*n+2]=63;producer[4*n+3]=255;}
            id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> enc=[cb computeCommandEncoder];
            [enc setComputePipelineState:pipeline];[enc setTexture:texture atIndex:0];[enc setBuffer:result offset:0 atIndex:0];
            [enc dispatchThreads:MTLSizeMake(w,h,1) threadsPerThreadgroup:MTLSizeMake(16,8,1)];[enc endEncoding];[cb commit];[cb waitUntilCompleted];
            assert(cb.status==MTLCommandBufferStatusCompleted);
            const uint8_t *out=result.contents;
            for(size_t n=0;n<(size_t)w*h;n++)assert(out[4*n]==63 && out[4*n+1]==42 && out[4*n+2]==21+pass && out[4*n+3]==255);
        }
        munmap(producer,sharedBytes);texture=nil;hv_fb_display_restore_firmware(fb);hv_fb_snapshot_release(&sharedFrame);
        puts("PASS: exported shared FD becomes Metal scanout with 4KB resource size on a 16KB host; live producer updates reach the GPU without a frame upload");
        pthread_mutex_destroy(&fb->mu);
        free(fb); free(machine);
        munmap(ram, size);
        printf("PASS: %s executes both shared-memory GPU reads, including CPU updates, every pixel verified; stale/unaligned/out-of-range frames rejected\n", device.name.UTF8String);
    }
    return 0;
}
