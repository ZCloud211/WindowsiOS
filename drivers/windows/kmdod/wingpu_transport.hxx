/* SPDX-License-Identifier: MIT */
#pragma once
#include "../../../gpu/wingpu_protocol.h"
#include "../../../gpu/wingpu_probe.h"

struct WgTransport {
    volatile ULONG *Registers;
    PVOID Allocation;
    PHYSICAL_ADDRESS Physical;
    FAST_MUTEX Lock;
    UINT64 Fence;
};
NTSTATUS WgTransportStart(WgTransport *g, PCM_RESOURCE_LIST resources);
VOID WgTransportStop(WgTransport *g);
NTSTATUS WgTransportProbe(WgTransport *g, WgProbeEscape *probe);
NTSTATUS APIENTRY WgDdiEscape(CONST HANDLE adapter, CONST DXGKARG_ESCAPE *escape);
