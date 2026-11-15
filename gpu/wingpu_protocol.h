/* SPDX-License-Identifier: MIT
 * WindowsiOS GPU transport ABI. No Windows or Apple SDK types cross this boundary.
 */
#ifndef WINIOS_GPU_PROTOCOL_H
#define WINIOS_GPU_PROTOCOL_H
#include <stdint.h>

#define WG_MAGIC 0x47504957u /* WIPG, little endian */
#define WG_ABI_VERSION 1u
#define WG_PCI_VENDOR 0x1b36u
#define WG_PCI_DEVICE 0xff10u /* Local experimental ID, not an assigned vendor product. */
#define WG_MAX_COMMANDS 256u
#define WG_MAX_TARGETS 16u
#define WG_MAX_DIMENSION 4096u
#define WG_MAX_TARGET_BYTES (64u << 20)

enum {
    WG_REG_MAGIC = 0x00, WG_REG_ABI = 0x04, WG_REG_CAPS = 0x08,
    WG_REG_BUSY = 0x0c, WG_REG_BATCH_LO = 0x10, WG_REG_BATCH_HI = 0x14,
    WG_REG_BATCH_BYTES = 0x18, WG_REG_SUBMIT = 0x1c,
    WG_REG_COMPLETION_LO = 0x20, WG_REG_COMPLETION_HI = 0x24,
    WG_REG_LAST_ERROR = 0x28, WG_REG_ENABLE = 0x2c
};
enum { WG_CAP_GPU_BATCH = 1u, WG_CAP_OFFSCREEN_BGRA8 = 2u };
enum { WG_OP_CREATE = 1u, WG_OP_CLEAR = 2u, WG_OP_TEST_TRIANGLE = 3u, WG_OP_DESTROY = 4u };
enum { WG_FORMAT_BGRA8 = 1u };
enum { WG_PENDING = 0u, WG_COMPLETE = 1u, WG_FAILED = 2u };
enum { WG_OK = 0u, WG_ERR_ABI = 1u, WG_ERR_BOUNDS = 2u, WG_ERR_COMMAND = 3u,
       WG_ERR_RESOURCE = 4u, WG_ERR_METAL = 5u, WG_ERR_BUSY = 6u };

typedef struct WgBatchHeader {
    uint32_t magic, version, command_count, reserved;
    uint64_t fence, reserved2;
} WgBatchHeader;

/* 64-byte descriptors are batched behind one doorbell, never one exit per draw.
 * This first ABI advertises bring-up operations only, no Direct3D feature level. */
typedef struct WgCommand {
    uint32_t opcode, resource, reserved[2];
    union {
        struct { uint32_t width, height, format, reserved[9]; } create;
        struct { uint32_t rgba8, reserved[11]; } clear;
        uint64_t words[6];
    } args;
} WgCommand;

typedef struct WgCompletion {
    uint32_t state, error;
    uint64_t fence, gpu_nanoseconds;
    uint64_t reserved[5];
} WgCompletion;

/* Private escape is restricted to GPU bring-up, not a UMD rendering DDI. */
typedef struct WgProbeEscape {
    uint32_t magic, version, error, completion_state;
    uint64_t fence, gpu_nanoseconds;
} WgProbeEscape;

#if defined(__cplusplus)
static_assert(sizeof(WgBatchHeader) == 32, "GPU batch ABI");
static_assert(sizeof(WgCommand) == 64, "GPU command ABI");
static_assert(sizeof(WgCompletion) == 64, "GPU completion ABI");
#else
_Static_assert(sizeof(WgBatchHeader) == 32, "GPU batch ABI");
_Static_assert(sizeof(WgCommand) == 64, "GPU command ABI");
_Static_assert(sizeof(WgCompletion) == 64, "GPU completion ABI");
#endif
#endif
