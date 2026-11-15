/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>
#include "../../../gpu/wingpu_protocol.h"
#include "../../../gpu/wingpu_probe.h"

struct WgTransport {
    volatile ULONG *Registers;
    PVOID Allocation;
    PHYSICAL_ADDRESS Physical;
    KMUTEX Lock;
    UINT64 Fence;
    BOOLEAN Enabled;
};
VOID WgTransportInitialize(WgTransport *g);
NTSTATUS WgTransportMap(WgTransport *g, const CM_PARTIAL_RESOURCE_DESCRIPTOR *resource);
NTSTATUS WgTransportResume(WgTransport *g);
NTSTATUS WgTransportPause(WgTransport *g);
VOID WgTransportStop(WgTransport *g);
NTSTATUS WgTransportProbe(WgTransport *g, WgProbeEscape *probe);
