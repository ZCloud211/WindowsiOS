/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wingpu.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct HvWinGpu {
    HvPciDev pci;
    HvMachine *m;
    HvWinGpuMetal *metal;
    pthread_mutex_t mu;
    uint64_t batch_pa, completion_pa, last_fence;
    uint32_t batch_bytes, enabled, busy, last_error;
    int gpu_done;
    uint64_t submitted_count, completed_count, failed_count;
    WgCompletion completed;
};

static void *ram_slice(HvMachine *m, uint64_t pa, uint64_t bytes, int writing) {
    for (int n = 0; n < m->nram; n++) {
        HvRam *r = &m->ram[n];
        if (pa < r->pa || pa - r->pa >= r->size || bytes > r->size - (pa - r->pa)) continue;
        if (writing && r->readonly) return NULL;
        return r->host + (pa - r->pa);
    }
    return NULL;
}

/* The Metal callback never mutates guest RAM or the JIT from a GPU thread. */
static void finished(void *context, uint32_t error, uint64_t ns) {
    HvWinGpu *g = context;
    pthread_mutex_lock(&g->mu);
    g->completed.error = error;
    g->completed.gpu_nanoseconds = ns;
    g->completed.state = error ? WG_FAILED : WG_COMPLETE;
    g->gpu_done = 1;
    pthread_mutex_unlock(&g->mu);
}

/* Pumped on the VM/device thread. Publish state last, after the result/fence. */
static void complete_locked(HvWinGpu *g) {
    if (!g->gpu_done) return;
    WgCompletion result = g->completed;
    uint32_t state = result.state;
    result.state = WG_PENDING;
    WgCompletion *destination = ram_slice(g->m, g->completion_pa, sizeof result, 1);
    if (destination) {
        hv_phys_write(g->m, g->completion_pa, &result, sizeof result);
        __atomic_store_n(&destination->state, state, __ATOMIC_RELEASE);
    }
    g->last_error = result.error;
    g->completed_count++;
    if (result.error) g->failed_count++;
    g->gpu_done = 0;
    g->busy = 0;
}

void hv_wingpu_poll(HvWinGpu *g) {
    if (!g) return;
    pthread_mutex_lock(&g->mu);
    complete_locked(g);
    pthread_mutex_unlock(&g->mu);
}

void hv_wingpu_stats(HvWinGpu *g, HvWinGpuStats *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof *stats);
    if (!g) return;
    pthread_mutex_lock(&g->mu);
    *stats = (HvWinGpuStats){
        .submitted = g->submitted_count, .completed = g->completed_count,
        .failed = g->failed_count, .last_fence = g->last_fence,
        .gpu_nanoseconds = g->completed.gpu_nanoseconds,
        .enabled = g->enabled, .busy = g->busy, .last_error = g->last_error
    };
    pthread_mutex_unlock(&g->mu);
}

static uint32_t submit_locked(HvWinGpu *g) {
    if (g->busy) return WG_ERR_BUSY;
    if (!g->enabled) return WG_ERR_ABI;
    if ((g->batch_pa & 63) || (g->completion_pa & 63) ||
        g->batch_bytes < sizeof(WgBatchHeader) ||
        g->batch_bytes > sizeof(WgBatchHeader) + WG_MAX_COMMANDS * sizeof(WgCommand)) return WG_ERR_BOUNDS;
    const void *source = ram_slice(g->m, g->batch_pa, g->batch_bytes, 0);
    void *completion = ram_slice(g->m, g->completion_pa, sizeof(WgCompletion), 1);
    if (!source || !completion) return WG_ERR_BOUNDS;
    if (g->completion_pa < g->batch_pa + g->batch_bytes &&
        g->batch_pa < g->completion_pa + sizeof(WgCompletion)) return WG_ERR_BOUNDS;
    /* Small immutable descriptors are copied once per batch. Resource pixels
     * are not copied here; later resource-sharing commands have a separate ABI. */
    uint8_t *copy = malloc(g->batch_bytes);
    if (!copy) return WG_ERR_RESOURCE;
    memcpy(copy, source, g->batch_bytes);
    WgBatchHeader *h = (WgBatchHeader *)copy;
    uint32_t error = WG_ERR_ABI;
    if (h->magic != WG_MAGIC || h->version != WG_ABI_VERSION || h->reserved || h->reserved2) goto done;
    error = WG_ERR_BOUNDS;
    if (!h->command_count || h->command_count > WG_MAX_COMMANDS ||
        sizeof *h + (uint64_t)h->command_count * sizeof(WgCommand) != g->batch_bytes ||
        !h->fence || h->fence <= g->last_fence) goto done;
    g->completed = (WgCompletion){.fence = h->fence};
    error = hv_wingpu_metal_submit(g->metal, (const WgCommand *)(copy + sizeof *h), h->command_count, finished, g);
    if (error == WG_OK) {
        WgCompletion pending = {.fence = h->fence};
        hv_phys_write(g->m, g->completion_pa, &pending, sizeof pending);
        g->last_fence = h->fence;
        g->submitted_count++;
        g->busy = 1;
    }
done:
    free(copy);
    return error;
}

static uint64_t bar_read(HvPciDev *d, int bar, uint64_t off, unsigned size) {
    HvWinGpu *g = d->opaque;
    if (bar || size != 4 || (off & 3)) return 0;
    pthread_mutex_lock(&g->mu);
    complete_locked(g);
    uint32_t v = 0;
    switch (off) {
    case WG_REG_MAGIC: v = WG_MAGIC; break;
    case WG_REG_ABI: v = WG_ABI_VERSION; break;
    case WG_REG_CAPS: v = WG_CAP_GPU_BATCH | WG_CAP_OFFSCREEN_BGRA8; break;
    case WG_REG_BUSY: v = g->busy; break;
    case WG_REG_LAST_ERROR: v = g->last_error; break;
    case WG_REG_ENABLE: v = g->enabled; break;
    default: break;
    }
    pthread_mutex_unlock(&g->mu);
    return v;
}

static void bar_write(HvPciDev *d, int bar, uint64_t off, uint64_t value, unsigned size) {
    HvWinGpu *g = d->opaque;
    if (bar || size != 4 || (off & 3)) return;
    uint32_t v = (uint32_t)value;
    pthread_mutex_lock(&g->mu);
    complete_locked(g);
    if (g->busy) { g->last_error = WG_ERR_BUSY; goto done; }
    switch (off) {
    case WG_REG_BATCH_LO: g->batch_pa = (g->batch_pa & ~UINT64_C(0xffffffff)) | v; break;
    case WG_REG_BATCH_HI: g->batch_pa = (g->batch_pa & UINT64_C(0xffffffff)) | ((uint64_t)v << 32); break;
    case WG_REG_COMPLETION_LO: g->completion_pa = (g->completion_pa & ~UINT64_C(0xffffffff)) | v; break;
    case WG_REG_COMPLETION_HI: g->completion_pa = (g->completion_pa & UINT64_C(0xffffffff)) | ((uint64_t)v << 32); break;
    case WG_REG_BATCH_BYTES: g->batch_bytes = v; break;
    case WG_REG_ENABLE:
        g->enabled = v == 1;
        if (!g->enabled) g->last_fence = 0;
        break;
    case WG_REG_SUBMIT:
        if (v == 1) g->last_error = submit_locked(g);
        break;
    default: break;
    }
done:
    pthread_mutex_unlock(&g->mu);
}

HvWinGpu *hv_wingpu_create(HvMachine *m, HvPcie *bus) {
    HvWinGpu *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->metal = hv_wingpu_metal_create();
    if (!g->metal) { free(g); return NULL; }
    g->m = m;
    pthread_mutex_init(&g->mu, NULL);
    /* Non-boot 3D controller: do not compete with the working firmware display. */
    hv_pci_init(&g->pci, WG_PCI_VENDOR, WG_PCI_DEVICE, 0x03020001, WG_PCI_VENDOR, WG_PCI_DEVICE);
    hv_pci_add_bar(&g->pci, 0, 4096, 0);
    hv_pci_add_pm_cap(&g->pci, 0x40);
    hv_pci_add_pcie_cap(&g->pci, 0x50);
    g->pci.opaque = g; g->pci.bar_read = bar_read; g->pci.bar_write = bar_write;
    if (hv_pcie_attach(bus, &g->pci) < 0) { hv_wingpu_destroy(g); return NULL; }
    return g;
}

HvPciDev *hv_wingpu_pci(HvWinGpu *g) { return g ? &g->pci : NULL; }
int hv_wingpu_test_readback(HvWinGpu *g, uint32_t resource, void *pixels, size_t bytes) {
    return g ? hv_wingpu_metal_test_readback(g->metal, resource, pixels, bytes) : -1;
}
void hv_wingpu_destroy(HvWinGpu *g) {
    if (!g) return;
    hv_wingpu_metal_destroy(g->metal);
    if (g->pci.bus && g->pci.slot >= 0 && g->pci.bus->dev[g->pci.slot] == &g->pci)
        g->pci.bus->dev[g->pci.slot] = NULL;
    pthread_mutex_destroy(&g->mu);
    free(g);
}
