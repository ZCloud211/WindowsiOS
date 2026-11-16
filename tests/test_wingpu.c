/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "wingpu.h"
#include "../gpu/wingpu_probe.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RAM UINT64_C(0x40000000)
#define ECAM UINT64_C(0x4010000000)
#define BAR UINT64_C(0x10100000)
#define BATCH (RAM + 0x1000)
#define RESULT (RAM + 0x8000)

static void write32(HvMachine *m, uint64_t pa, uint32_t value) { assert(!hv_phys_write(m, pa, &value, 4)); }
static uint32_t read32(HvMachine *m, uint64_t pa) { uint32_t v = 0; assert(!hv_phys_read(m, pa, &v, 4)); return v; }
static void pa_register(HvMachine *m, uint32_t reg, uint64_t pa) {
    write32(m, BAR + reg, (uint32_t)pa); write32(m, BAR + reg + 4, (uint32_t)(pa >> 32));
}
static void submit(HvMachine *m, uint32_t bytes) {
    write32(m, BAR + WG_REG_BATCH_BYTES, bytes);
    write32(m, BAR + WG_REG_SUBMIT, 1);
}
static void wait_complete_at(HvMachine *m, uint64_t fence, uint64_t physical) {
    WgCompletion *c = hv_phys_ptr(m, physical, sizeof *c);
    struct timespec delay = {.tv_nsec = 1000000};
    for (int n = 0; n < 5000; n++) {
        read32(m, BAR + WG_REG_BUSY); /* Same device completion pump used by Windows. */
        uint32_t state = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
        if (state != WG_PENDING) {
            assert(state == WG_COMPLETE && c->error == WG_OK && c->fence == fence);
            return;
        }
        nanosleep(&delay, NULL);
    }
    fprintf(stderr, "GPU completion timeout\n"); abort();
}
static void wait_complete(HvMachine *m, uint64_t fence) { wait_complete_at(m, fence, RESULT); }

int main(void) {
    HvMachine *m = calloc(1, sizeof *m);
    pthread_mutex_init(&m->dev_mu, NULL);
    uint8_t *ram = calloc(1, 1 << 20);
    assert(!hv_machine_add_ram(m, RAM, 1 << 20, ram, 0));
    HvPcie *bus = hv_pcie_create(m, NULL, ECAM, 0x10000000, 0x10000000, 0x1000000, 35);
    HvWinGpu *gpu = hv_wingpu_create(m, bus);
    if (!gpu) { fprintf(stderr, "A working local Metal GPU is required; verification not run.\n"); return 77; }
    HvPciDev *pci = hv_wingpu_pci(gpu);
    uint64_t config = ECAM + ((uint64_t)pci->slot << 15);
    assert(read32(m, config) == (WG_PCI_DEVICE << 16 | WG_PCI_VENDOR));
    assert(read32(m, config + 8) == 0x03020001); /* not a boot VGA adapter */
    write32(m, config + 0x10, UINT32_MAX);
    assert((read32(m, config + 0x10) & ~15u) == 0xfffff000u);
    write32(m, config + 0x10, BAR);
    uint16_t memory_enable = 2;
    assert(!hv_phys_write(m, config + 4, &memory_enable, 2));
    assert(read32(m, BAR + WG_REG_MAGIC) == WG_MAGIC);
    assert(read32(m, BAR + WG_REG_CAPS) == (WG_CAP_GPU_BATCH | WG_CAP_OFFSCREEN_BGRA8));
    pa_register(m, WG_REG_BATCH_LO, BATCH); pa_register(m, WG_REG_COMPLETION_LO, RESULT);
    write32(m, BAR + WG_REG_ENABLE, 1);
    void *memory = hv_phys_ptr(m, BATCH, 4096);
    uint32_t bytes = wg_encode_probe(memory, 4096, 1);
    assert(bytes);
    submit(m, bytes); assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_OK);
    wait_complete(m, 1);
    puts("PASS: shared Windows KMD batch encoder (host invocation) -> PCI BAR -> Metal clear/draw -> guest completion/fence");

    bytes = wg_encode_probe(memory, 4096, 2);
    WgBatchHeader *h = memory;
    h->magic = 0;
    submit(m, bytes); assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_ERR_ABI);
    h->magic = WG_MAGIC;
    WgCommand *c = (WgCommand *)(h + 1);
    h->command_count = 3;
    for (int n = 0; n < 3; n++) c[n].resource = 2;
    c[1].args.clear.rgba8 = 0xffff0000; /* blue clear */
    bytes = sizeof *h + 3 * sizeof *c;
    submit(m, bytes); assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_OK); wait_complete(m, 2);
    uint8_t *image = malloc(256 * 256 * 4);
    assert(!hv_wingpu_test_readback(gpu, 2, image, 256 * 256 * 4));
    assert(image[0] == 255 && image[1] == 0 && image[2] == 0 && image[3] == 255);
    const uint8_t *center = image + (128 * 256 + 128) * 4;
    assert(center[0] == 0 && center[1] == 0 && center[2] == 255 && center[3] == 255);
    puts("PASS: M1 GPU rasterizes a red triangle over a blue target; readback is host-test-only");

    bytes = wg_encode_probe(memory, 4096, 3);
    c[0].resource = 3; c[1].resource = 3; c[1].opcode = 0xffffffffu;
    submit(m, bytes); assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_ERR_COMMAND);
    assert(hv_wingpu_test_readback(gpu, 3, image, 256 * 256 * 4));
    assert(!hv_wingpu_test_readback(gpu, 2, image, 256 * 256 * 4));
    puts("PASS: malformed trailing commands cannot commit partially-created GPU resources");

    bytes = wg_encode_probe(memory, 4096, 3);
    h->command_count = UINT32_MAX;
    submit(m, bytes); assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_ERR_BOUNDS);
    h->command_count = 4;
    pa_register(m, WG_REG_BATCH_LO, UINT64_MAX - 63);
    submit(m, bytes); assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_ERR_BOUNDS);
    pa_register(m, WG_REG_BATCH_LO, BATCH);
    pa_register(m, WG_REG_COMPLETION_LO, BATCH);
    submit(m, bytes); assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_ERR_BOUNDS);
    pa_register(m, WG_REG_COMPLETION_LO, RESULT);
    submit(m, bytes); assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_OK); wait_complete(m, 3);
    submit(m, bytes); assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_ERR_BOUNDS);
    puts("PASS: invalid counts, wrapped addresses, overlapping command/completion memory and replayed fences rejected");

    memset(memory, 0, 4096);
    h->magic = WG_MAGIC; h->version = WG_ABI_VERSION; h->fence = 4; h->command_count = 10;
    for (int n = 0; n < 5; n++) {
        c[2*n].opcode = WG_OP_CREATE; c[2*n].resource = 3;
        c[2*n].args.create.width = c[2*n].args.create.height = 2048;
        c[2*n].args.create.format = WG_FORMAT_BGRA8;
        c[2*n+1].opcode = WG_OP_DESTROY; c[2*n+1].resource = 3;
    }
    submit(m, sizeof *h + 10 * sizeof *c);
    assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_ERR_RESOURCE);
    assert(hv_wingpu_test_readback(gpu, 3, image, 256 * 256 * 4));
    puts("PASS: resource churn cannot bypass the GPU resident-memory budget with create/destroy pairs");

    write32(m, BAR + WG_REG_ENABLE, 0);
    bytes = wg_encode_probe(memory, 4096, 4);
    submit(m, bytes); assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_ERR_ABI);
    write32(m, BAR + WG_REG_ENABLE, 1);
    memset(hv_phys_ptr(m, RESULT, sizeof(WgCompletion)), 0, sizeof(WgCompletion));
    bytes = wg_encode_probe(memory, 4096, 1); /* D0 resume starts a new fence epoch. */
    submit(m, bytes); assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_OK);
    wait_complete(m, 1);
    puts("PASS: disable/resume permits a new fence epoch without stale completion reuse");
    /* Windows may allocate DMA pages above 4GiB with the 4GiB RAM profile. */
    uint8_t *high_ram = calloc(1, 1 << 20);
    const uint64_t high_batch = UINT64_C(0x100001000), high_result = UINT64_C(0x100008000);
    assert(!hv_machine_add_ram(m, UINT64_C(0x100000000), 1 << 20, high_ram, 0));
    pa_register(m, WG_REG_BATCH_LO, high_batch);
    pa_register(m, WG_REG_COMPLETION_LO, high_result);
    bytes = wg_encode_probe(hv_phys_ptr(m, high_batch, 4096), 4096, 2);
    submit(m, bytes); assert(read32(m, BAR + WG_REG_LAST_ERROR) == WG_OK);
    wait_complete_at(m, 2, high_result);
    HvWinGpuStats stats;
    hv_wingpu_stats(gpu, &stats);
    assert(stats.submitted == stats.completed && stats.completed >= 2 && !stats.failed &&
           stats.last_fence == 2 && !stats.busy && !stats.last_error);
    puts("PASS: command and completion physical addresses above 4GiB preserve high register bits");
    hv_wingpu_destroy(gpu);
    free(image); free(bus);
    for (int n = 0; n < m->nram; n++) { free(m->ram[n].code_pages); free(m->ram[n].page_gen); free(m->ram[n].whint); }
    free(ram); free(high_ram); pthread_mutex_destroy(&m->dev_mu); free(m);
    puts("PASS: experimental GPU stays disabled unless explicitly enabled; no iPad operation performed");
    return 0;
}
