/* SPDX-License-Identifier: MIT
 * Windows-only public diagnostic interface; not part of the PCI wire ABI.
 * Include Windows/WDK IOCTL and GUID definitions before this header.
 */
#ifndef WINIOS_GPU_IOCTL_H
#define WINIOS_GPU_IOCTL_H
#include "wingpu_protocol.h"
/* {8F88897E-CC81-4A9E-A957-7436B883C69A} */
DEFINE_GUID(GUID_DEVINTERFACE_WINIOS_GPU_PROBE,
    0x8f88897e, 0xcc81, 0x4a9e, 0xa9, 0x57, 0x74, 0x36, 0xb8, 0x83, 0xc6, 0x9a);
#define WG_DIAGNOSTIC_DEVICE_TYPE 0x8000
#define IOCTL_WG_PROBE CTL_CODE(WG_DIAGNOSTIC_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
typedef struct WgDiagnosticResult {
    WgProbeEscape probe;
    wg_u32 ntstatus, reserved;
} WgDiagnosticResult;
#endif
