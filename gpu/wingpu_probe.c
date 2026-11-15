/* SPDX-License-Identifier: MIT */
#include "wingpu_probe.h"

uint32_t wg_encode_probe(void *batch, uint32_t capacity, uint64_t fence) {
    const uint32_t bytes = sizeof(WgBatchHeader) + 4 * sizeof(WgCommand);
    if (!batch || ((uintptr_t)batch & 7) || capacity < bytes || !fence) return 0;
    unsigned char *raw = batch;
    for (uint32_t n = 0; n < bytes; n++) raw[n] = 0;
    WgBatchHeader *h = batch;
    WgCommand *c = (WgCommand *)(h + 1);
    h->magic = WG_MAGIC; h->version = WG_ABI_VERSION;
    h->command_count = 4; h->fence = fence;
    c[0].opcode = WG_OP_CREATE; c[0].resource = 1;
    c[0].args.create.width = c[0].args.create.height = 256;
    c[0].args.create.format = WG_FORMAT_BGRA8;
    c[1].opcode = WG_OP_CLEAR; c[1].resource = 1;
    c[1].args.clear.rgba8 = 0xff000000;
    c[2].opcode = WG_OP_TEST_TRIANGLE; c[2].resource = 1;
    c[3].opcode = WG_OP_DESTROY; c[3].resource = 1;
    return bytes;
}
