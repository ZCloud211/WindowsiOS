/* SPDX-License-Identifier: MIT
 * Windows ARM64 WDDM bring-up transport. This is a diagnostic path, not a
 * Direct3D UMD; its bounded synchronous wait is never the production scheduler.
 */
#include "bdd.hxx"

static ULONG WgRead(WgTransport *g, ULONG reg) {
    return READ_REGISTER_ULONG(g->Registers + reg / sizeof(ULONG));
}
static VOID WgWrite(WgTransport *g, ULONG reg, ULONG value) {
    WRITE_REGISTER_ULONG(g->Registers + reg / sizeof(ULONG), value);
}
static BOOLEAN WgWaitIdle(WgTransport *g) {
    LARGE_INTEGER delay;
    delay.QuadPart = -10000; /* Probe-only: yield for 1ms, at most one second. */
    for (ULONG n = 0; n < 1000; n++) {
        if (!WgRead(g, WG_REG_BUSY)) return TRUE;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    return FALSE;
}

NTSTATUS WgTransportStart(WgTransport *g, PCM_RESOURCE_LIST resources) {
    PAGED_CODE();
    RtlZeroMemory(g, sizeof *g);
    ExInitializeFastMutex(&g->Lock);
    if (!resources) return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (ULONG full = 0; full < resources->Count && !g->Registers; full++) {
        const CM_PARTIAL_RESOURCE_LIST *list = &resources->List[full].PartialResourceList;
        for (ULONG i = 0; i < list->Count; i++) {
            const CM_PARTIAL_RESOURCE_DESCRIPTOR *r = &list->PartialDescriptors[i];
            if (r->Type != CmResourceTypeMemory || r->u.Memory.Length != 4096) continue;
            volatile ULONG *map = static_cast<volatile ULONG *>(MmMapIoSpaceEx(r->u.Memory.Start, 4096, PAGE_READWRITE | PAGE_NOCACHE));
            if (!map) continue;
            g->Registers = map;
            if (WgRead(g, WG_REG_MAGIC) == WG_MAGIC && WgRead(g, WG_REG_ABI) == WG_ABI_VERSION) break;
            MmUnmapIoSpace(const_cast<ULONG *>(map), 4096);
            g->Registers = NULL;
        }
    }
    if (!g->Registers) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!WgWaitIdle(g)) { WgTransportStop(g); return STATUS_IO_TIMEOUT; }
    PHYSICAL_ADDRESS low = {}, high, boundary = {};
    high.QuadPart = MAXLONGLONG;
    g->Allocation = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE * 2, low, high, boundary, MmCached);
    if (!g->Allocation) { WgTransportStop(g); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(g->Allocation, PAGE_SIZE * 2);
    g->Physical = MmGetPhysicalAddress(g->Allocation);
    UINT64 completion = g->Physical.QuadPart + PAGE_SIZE;
    WgWrite(g, WG_REG_BATCH_LO, g->Physical.LowPart);
    WgWrite(g, WG_REG_BATCH_HI, g->Physical.HighPart);
    WgWrite(g, WG_REG_COMPLETION_LO, static_cast<ULONG>(completion));
    WgWrite(g, WG_REG_COMPLETION_HI, static_cast<ULONG>(completion >> 32));
    WgWrite(g, WG_REG_ENABLE, 1);
    return STATUS_SUCCESS;
}

NTSTATUS WgTransportProbe(WgTransport *g, WgProbeEscape *probe) {
    PAGED_CODE();
    if (!g->Registers || !g->Allocation) return STATUS_DEVICE_NOT_READY;
    ExAcquireFastMutex(&g->Lock);
    NTSTATUS status = STATUS_SUCCESS;
    if (!WgWaitIdle(g)) { status = STATUS_IO_TIMEOUT; goto finish; }
    {
        WgBatchHeader *header = static_cast<WgBatchHeader *>(g->Allocation);
        volatile WgCompletion *result = reinterpret_cast<volatile WgCompletion *>(static_cast<UCHAR *>(g->Allocation) + PAGE_SIZE);
        RtlZeroMemory(g->Allocation, PAGE_SIZE * 2);
        ULONG batchBytes = wg_encode_probe(g->Allocation, PAGE_SIZE, ++g->Fence);
        if (!batchBytes) { status = STATUS_INVALID_PARAMETER; goto finish; }
        KeMemoryBarrier();
        WgWrite(g, WG_REG_BATCH_BYTES, batchBytes);
        WgWrite(g, WG_REG_SUBMIT, 1);
        ULONG error = WgRead(g, WG_REG_LAST_ERROR);
        if (error != WG_OK) {
            probe->error = error; status = STATUS_DEVICE_PROTOCOL_ERROR; goto finish;
        }
        if (!WgWaitIdle(g)) { status = STATUS_IO_TIMEOUT; goto finish; }
        KeMemoryBarrier();
        probe->completion_state = result->state;
        probe->error = result->error;
        probe->fence = result->fence;
        probe->gpu_nanoseconds = result->gpu_nanoseconds;
        if (result->state != WG_COMPLETE || result->fence != header->fence || result->error != WG_OK)
            status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
finish:
    ExReleaseFastMutex(&g->Lock);
    return status;
}

VOID WgTransportStop(WgTransport *g) {
    PAGED_CODE();
    if (!g->Registers) return;
    ExAcquireFastMutex(&g->Lock);
    BOOLEAN idle = WgWaitIdle(g);
    if (idle) {
        WgWrite(g, WG_REG_ENABLE, 0);
        if (g->Allocation) MmFreeContiguousMemory(g->Allocation);
    } else {
        /* Quarantine these 8KiB until reboot rather than allow late GPU DMA
         * to corrupt memory reused by Windows. This is a bring-up-only driver. */
        DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "WinIOS GPU: timeout; completion pages retained\n");
    }
    g->Allocation = NULL;
    MmUnmapIoSpace(const_cast<ULONG *>(g->Registers), 4096);
    g->Registers = NULL;
    ExReleaseFastMutex(&g->Lock);
}

NTSTATUS APIENTRY WgDdiEscape(CONST HANDLE adapter, CONST DXGKARG_ESCAPE *escape) {
    PAGED_CODE();
    if (!adapter || !escape || !escape->Flags.HardwareAccess || !escape->pPrivateDriverData ||
        escape->PrivateDriverDataSize != sizeof(WgProbeEscape)) return STATUS_INVALID_PARAMETER;
    WgProbeEscape *probe = static_cast<WgProbeEscape *>(escape->pPrivateDriverData);
    if (probe->magic != WG_MAGIC || probe->version != WG_ABI_VERSION) return STATUS_INVALID_PARAMETER;
    probe->error = WG_OK; probe->completion_state = WG_PENDING;
    probe->fence = probe->gpu_nanoseconds = 0;
    BASIC_DISPLAY_DRIVER *driver = static_cast<BASIC_DISPLAY_DRIVER *>(adapter);
    return WgTransportProbe(driver->GpuTransport(), probe);
}
