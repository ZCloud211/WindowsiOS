/* SPDX-License-Identifier: MIT
 * Read-only selection of our adapter, followed by its fixed offscreen GPU
 * self-test. Does not change display modes, install a driver or enable test signing.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <wchar.h>
#include "../../../gpu/wingpu_protocol.h"

int wmain(void) {
    for (DWORD index = 0;; index++) {
        DISPLAY_DEVICEW display = {0};
        display.cb = sizeof display;
        if (!EnumDisplayDevicesW(NULL, index, &display, 0)) break;
        if (!wcsstr(display.DeviceID, L"VEN_1B36&DEV_FF10")) continue;
        D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open = {0};
        wcscpy_s(open.DeviceName, CCHDEVICENAME, display.DeviceName);
        NTSTATUS status = D3DKMTOpenAdapterFromGdiDisplayName(&open);
        if (status < 0) { printf("Open adapter failed: 0x%08lx\n", (unsigned long)status); return 1; }
        WgProbeEscape probe = {0};
        probe.magic = WG_MAGIC; probe.version = WG_ABI_VERSION;
        D3DKMT_ESCAPE escape = {0};
        escape.hAdapter = open.hAdapter;
        escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
        escape.Flags.HardwareAccess = 1;
        escape.pPrivateDriverData = &probe;
        escape.PrivateDriverDataSize = sizeof probe;
        status = D3DKMTEscape(&escape);
        D3DKMT_CLOSEADAPTER close = {0}; close.hAdapter = open.hAdapter;
        D3DKMTCloseAdapter(&close);
        printf("status=0x%08lx state=%u error=%u fence=%llu gpu_ns=%llu\n",
               (unsigned long)status, probe.completion_state, probe.error,
               (unsigned long long)probe.fence, (unsigned long long)probe.gpu_nanoseconds);
        if (status < 0 || probe.completion_state != WG_COMPLETE || probe.error || !probe.fence) return 1;
        puts("GPU transport self-test completed. This does not claim Direct3D acceleration.");
        return 0;
    }
    fputs("WindowsiOS experimental GPU adapter was not found or has not started.\n", stderr);
    return 2;
}
