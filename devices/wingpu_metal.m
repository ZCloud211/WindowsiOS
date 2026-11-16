/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "wingpu_metal.h"
#include <stdlib.h>
#include <string.h>

/* Henge-style host Metal execution, independent of its macOS private protocol.
 * Targets stay in GPU-private tiled textures. Production submissions never
 * read pixels back to the CPU. TEST_TRIANGLE is a transport/renderer probe,
 * not an implementation of the Windows Direct3D user-mode driver. */
@interface WgMetalBackend : NSObject {
@public
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> probePipeline;
    NSMutableDictionary<NSNumber *, id<MTLTexture>> *targets;
    dispatch_group_t pending;
}
@end
@implementation WgMetalBackend
@end

static const uint64_t handle_magic = UINT64_C(0x574750554d45544c);
struct HvWinGpuMetal { uint64_t magic; void *backend; };

HvWinGpuMetal *hv_wingpu_metal_create(void) {
    @autoreleasepool {
        WgMetalBackend *b = [WgMetalBackend new];
        b->device = MTLCreateSystemDefaultDevice();
        if (!b->device) return NULL;
        b->queue = [b->device newCommandQueue];
        b->pending = dispatch_group_create();
        b->targets = [NSMutableDictionary dictionary];
        NSError *error = nil;
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; };"
            "vertex V probe_vs(uint i [[vertex_id]]) {"
            "const float2 p[3] = {float2(-0.8,-0.8),float2(0.8,-0.8),float2(0,0.8)};"
            "V v; v.p=float4(p[i],0,1); return v; }"
            "fragment float4 probe_fs() { return float4(1,0,0,1); }";
        id<MTLLibrary> lib = [b->device newLibraryWithSource:source options:nil error:&error];
        if (!lib || !b->queue) return NULL;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"probe_vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"probe_fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        b->probePipeline = [b->device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!b->probePipeline) return NULL;
        HvWinGpuMetal *gpu = calloc(1, sizeof *gpu);
        if (!gpu) return NULL;
        gpu->magic = handle_magic;
        gpu->backend = (__bridge_retained void *)b;
        return gpu;
    }
}

static int zero_words(const uint64_t *words, size_t count) {
    for (size_t n = 0; n < count; n++) if (words[n]) return 0;
    return 1;
}

uint32_t hv_wingpu_metal_submit(HvWinGpuMetal *gpu, const WgCommand *commands,
                                uint32_t count, HvWinGpuDone done, void *context) {
    if (!gpu || gpu->magic != handle_magic || !commands || !done || !count || count > WG_MAX_COMMANDS) return WG_ERR_BOUNDS;
    @autoreleasepool {
        WgMetalBackend *b = (__bridge WgMetalBackend *)gpu->backend;
        NSMutableDictionary<NSNumber *, id<MTLTexture>> *next = [b->targets mutableCopy];
        id<MTLTexture> resolved[WG_MAX_COMMANDS] = {nil};
        uint64_t resident_bytes = 0;
        for (id<MTLTexture> t in next.allValues) resident_bytes += t.width * t.height * 4;
        /* Resolve the complete batch before changing persistent resources or
         * committing GPU work, so a malformed trailing command has no effects. */
        for (uint32_t n = 0; n < count; n++) {
            const WgCommand *c = &commands[n];
            if (c->reserved[0] || c->reserved[1] || !c->resource || c->resource > WG_MAX_TARGETS) return WG_ERR_COMMAND;
            NSNumber *key = @(c->resource);
            id<MTLTexture> t = next[key];
            switch (c->opcode) {
            case WG_OP_CREATE: {
                uint32_t w = c->args.create.width, h = c->args.create.height;
                if (t || !w || !h || w > WG_MAX_DIMENSION || h > WG_MAX_DIMENSION ||
                    c->args.create.format != WG_FORMAT_BGRA8) return WG_ERR_RESOURCE;
                for (int k = 0; k < 9; k++) if (c->args.create.reserved[k]) return WG_ERR_COMMAND;
                uint64_t bytes = (uint64_t)w * h * 4;
                /* Destroyed textures remain retained by encoded commands until
                 * GPU completion. Count all allocations, including retired ones. */
                if (resident_bytes + bytes > WG_MAX_TARGET_BYTES) return WG_ERR_RESOURCE;
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
                td.storageMode = MTLStorageModePrivate;
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                t = [b->device newTextureWithDescriptor:td];
                if (!t) return WG_ERR_METAL;
                next[key] = t;
                resident_bytes += bytes;
                break;
            }
            case WG_OP_CLEAR:
                for (int k = 0; k < 11; k++) if (c->args.clear.reserved[k]) return WG_ERR_COMMAND;
                if (!t) return WG_ERR_RESOURCE;
                break;
            case WG_OP_TEST_TRIANGLE:
                if (!t || !zero_words(c->args.words, 6)) return WG_ERR_RESOURCE;
                break;
            case WG_OP_DESTROY:
                if (!t || !zero_words(c->args.words, 6)) return WG_ERR_RESOURCE;
                [next removeObjectForKey:key];
                break;
            default: return WG_ERR_COMMAND;
            }
            resolved[n] = t;
        }
        id<MTLCommandBuffer> cb = [b->queue commandBuffer];
        if (!cb) return WG_ERR_METAL;
        for (uint32_t n = 0; n < count; n++) {
            const WgCommand *c = &commands[n];
            if (c->opcode != WG_OP_CLEAR && c->opcode != WG_OP_TEST_TRIANGLE) continue;
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = resolved[n];
            rp.colorAttachments[0].loadAction = c->opcode == WG_OP_CLEAR ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            uint32_t rgba = c->args.clear.rgba8;
            rp.colorAttachments[0].clearColor = MTLClearColorMake((rgba & 255) / 255.0, ((rgba >> 8) & 255) / 255.0,
                ((rgba >> 16) & 255) / 255.0, (rgba >> 24) / 255.0);
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            if (!enc) return WG_ERR_METAL;
            if (c->opcode == WG_OP_TEST_TRIANGLE) {
                [enc setRenderPipelineState:b->probePipeline];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [enc endEncoding];
        }
        b->targets = next;
        dispatch_group_enter(b->pending);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            uint32_t error = completed.status == MTLCommandBufferStatusCompleted ? WG_OK : WG_ERR_METAL;
            double seconds = completed.GPUEndTime - completed.GPUStartTime;
            uint64_t ns = seconds > 0 ? (uint64_t)(seconds * 1e9) : 0;
            done(context, error, ns);
            dispatch_group_leave(b->pending);
        }];
        [cb commit];
        return WG_OK;
    }
}

int hv_wingpu_metal_test_readback(HvWinGpuMetal *gpu, uint32_t resource, void *pixels, size_t bytes) {
    if (!gpu || gpu->magic != handle_magic || !pixels) return -1;
    WgMetalBackend *b = (__bridge WgMetalBackend *)gpu->backend;
    dispatch_group_wait(b->pending, DISPATCH_TIME_FOREVER);
    id<MTLTexture> t = b->targets[@(resource)];
    if (!t || bytes != t.width * t.height * 4) return -1;
    NSUInteger pitch = (t.width * 4 + 255) & ~(NSUInteger)255;
    id<MTLBuffer> buffer = [b->device newBufferWithLength:pitch * t.height options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [b->queue commandBuffer];
    id<MTLBlitCommandEncoder> enc = [cb blitCommandEncoder];
    if (!buffer || !cb || !enc) return -1;
    [enc copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:buffer destinationOffset:0
    destinationBytesPerRow:pitch destinationBytesPerImage:pitch * t.height];
    [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) return -1;
    for (NSUInteger y = 0; y < t.height; y++)
        memcpy((uint8_t *)pixels + y * t.width * 4, (const uint8_t *)buffer.contents + y * pitch, t.width * 4);
    return 0;
}

void hv_wingpu_metal_destroy(HvWinGpuMetal *gpu) {
    if (!gpu) return;
    WgMetalBackend *b = (__bridge WgMetalBackend *)gpu->backend;
    dispatch_group_wait(b->pending, DISPATCH_TIME_FOREVER);
    gpu->magic = 0;
    CFBridgingRelease(gpu->backend);
    free(gpu);
}
