/* SPDX-License-Identifier: MIT
 * Fixed diagnostic batches only. No D3D scheduling or arbitrary DMA IOCTL.
 * All entry points run at PASSIVE_LEVEL; KMUTEX does not raise the IRQL.
 */
#include "transport.hxx"

static ULONG Read(WgTransport *g, ULONG reg) {
    return READ_REGISTER_ULONG(g->Registers + reg / sizeof(ULONG));
}
static VOID Write(WgTransport *g, ULONG reg, ULONG value) {
    WRITE_REGISTER_ULONG(g->Registers + reg / sizeof(ULONG), value);
}
static BOOLEAN WaitIdle(WgTransport *g) {
    LARGE_INTEGER delay;
    delay.QuadPart = -10000;
    for (ULONG n = 0; n < 1000; n++) {
        if (!Read(g, WG_REG_BUSY)) return TRUE;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    return FALSE;
}
static VOID Lock(WgTransport *g) {
    KeWaitForSingleObject(&g->Lock, Executive, KernelMode, FALSE, NULL);
}
VOID WgTransportInitialize(WgTransport *g) {
    RtlZeroMemory(g, sizeof *g);
    KeInitializeMutex(&g->Lock, 0);
}
NTSTATUS WgTransportMap(WgTransport *g, const CM_PARTIAL_RESOURCE_DESCRIPTOR *r) {
    PAGED_CODE();
    if (g->Registers || !r || r->Type != CmResourceTypeMemory || r->u.Memory.Length != 4096)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    g->Registers = static_cast<volatile ULONG *>(MmMapIoSpaceEx(r->u.Memory.Start, 4096, PAGE_READWRITE | PAGE_NOCACHE));
    if (!g->Registers) return STATUS_INSUFFICIENT_RESOURCES;
    if (Read(g, WG_REG_MAGIC) != WG_MAGIC || Read(g, WG_REG_ABI) != WG_ABI_VERSION ||
        (Read(g, WG_REG_CAPS) & (WG_CAP_GPU_BATCH | WG_CAP_OFFSCREEN_BGRA8)) !=
            (WG_CAP_GPU_BATCH | WG_CAP_OFFSCREEN_BGRA8)) {
        MmUnmapIoSpace(const_cast<ULONG *>(g->Registers), 4096);
        g->Registers = NULL;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (!WaitIdle(g)) { WgTransportStop(g); return STATUS_IO_TIMEOUT; }
    PHYSICAL_ADDRESS low = {}, high, boundary = {};
    high.QuadPart = MAXLONGLONG;
    g->Allocation = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE * 2, low, high, boundary, MmCached);
    if (!g->Allocation) { WgTransportStop(g); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(g->Allocation, PAGE_SIZE * 2);
    g->Physical = MmGetPhysicalAddress(g->Allocation);
    return STATUS_SUCCESS;
}
NTSTATUS WgTransportResume(WgTransport *g) {
    PAGED_CODE();
    Lock(g);
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (g->Registers && g->Allocation) {
        status = STATUS_IO_TIMEOUT;
        if (WaitIdle(g)) {
            Write(g, WG_REG_ENABLE, 0);
            UINT64 completion = g->Physical.QuadPart + PAGE_SIZE;
            Write(g, WG_REG_BATCH_LO, g->Physical.LowPart);
            Write(g, WG_REG_BATCH_HI, g->Physical.HighPart);
            Write(g, WG_REG_COMPLETION_LO, static_cast<ULONG>(completion));
            Write(g, WG_REG_COMPLETION_HI, static_cast<ULONG>(completion >> 32));
            g->Fence = 0;
            Write(g, WG_REG_ENABLE, 1);
            g->Enabled = TRUE;
            status = STATUS_SUCCESS;
        }
    }
    KeReleaseMutex(&g->Lock, FALSE);
    return status;
}
NTSTATUS WgTransportPause(WgTransport *g) {
    PAGED_CODE();
    Lock(g);
    NTSTATUS status = STATUS_SUCCESS;
    g->Enabled = FALSE;
    if (g->Registers) {
        if (WaitIdle(g)) Write(g, WG_REG_ENABLE, 0);
        else status = STATUS_IO_TIMEOUT;
    }
    KeReleaseMutex(&g->Lock, FALSE);
    return status;
}
NTSTATUS WgTransportProbe(WgTransport *g, WgProbeEscape *probe) {
    PAGED_CODE();
    Lock(g);
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (!g->Enabled || !g->Registers || !g->Allocation) goto finish;
    if (!WaitIdle(g)) { status = STATUS_IO_TIMEOUT; goto finish; }
    {
        volatile WgCompletion *result = reinterpret_cast<volatile WgCompletion *>(static_cast<UCHAR *>(g->Allocation) + PAGE_SIZE);
        RtlZeroMemory(g->Allocation, PAGE_SIZE * 2);
        ULONG bytes = wg_encode_probe(g->Allocation, PAGE_SIZE, ++g->Fence);
        if (!bytes) { status = STATUS_INVALID_PARAMETER; goto finish; }
        KeMemoryBarrier();
        Write(g, WG_REG_BATCH_BYTES, bytes);
        Write(g, WG_REG_SUBMIT, 1);
        probe->error = Read(g, WG_REG_LAST_ERROR);
        if (probe->error != WG_OK) { status = STATUS_DEVICE_PROTOCOL_ERROR; goto finish; }
        if (!WaitIdle(g)) { status = STATUS_IO_TIMEOUT; goto finish; }
        KeMemoryBarrier();
        probe->completion_state = result->state;
        probe->error = result->error;
        probe->fence = result->fence;
        probe->gpu_nanoseconds = result->gpu_nanoseconds;
        status = result->state == WG_COMPLETE && result->fence == g->Fence && result->error == WG_OK
            ? STATUS_SUCCESS : STATUS_DEVICE_PROTOCOL_ERROR;
    }
finish:
    KeReleaseMutex(&g->Lock, FALSE);
    return status;
}
VOID WgTransportStop(WgTransport *g) {
    PAGED_CODE();
    Lock(g);
    g->Enabled = FALSE;
    if (g->Registers) {
        BOOLEAN idle = WaitIdle(g);
        if (idle) {
            Write(g, WG_REG_ENABLE, 0);
            if (g->Allocation) MmFreeContiguousMemory(g->Allocation);
        } else if (g->Allocation) {
            /* Late completion must never write into reallocated Windows pages. */
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "WinIOS GPU: timeout; 8KiB retained until reboot\n");
        }
        g->Allocation = NULL;
        MmUnmapIoSpace(const_cast<ULONG *>(g->Registers), 4096);
        g->Registers = NULL;
    }
    KeReleaseMutex(&g->Lock, FALSE);
}
