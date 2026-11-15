/* SPDX-License-Identifier: MIT
 * Fixed offscreen GPU tests through an administrator-only PCI interface.
 * Does not install drivers, modify Windows policy, or change display modes.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "../../../gpu/wingpu_ioctl.h"

static int reject_invalid(HANDLE device) {
    WgProbeEscape input = {0};
    WgDiagnosticResult result;
    DWORD bytes;
    input.magic = WG_MAGIC; input.version = WG_ABI_VERSION;
    if (DeviceIoControl(device, IOCTL_WG_PROBE, &input, sizeof input - 1,
                        &result, sizeof result, &bytes, NULL)) return 1;
    if (GetLastError() != ERROR_BAD_LENGTH) return 1;
    input.magic = 0;
    if (DeviceIoControl(device, IOCTL_WG_PROBE, &input, sizeof input,
                        &result, sizeof result, &bytes, NULL)) return 1;
    if (GetLastError() != ERROR_INVALID_PARAMETER) return 1;
    puts("PASS: invalid request length and ABI rejected before GPU submission.");
    return 0;
}
static int run(HANDLE device, unsigned count) {
    if (reject_invalid(device)) { fputs("Invalid-request validation failed.\n", stderr); return 1; }
    unsigned long long lastFence = 0;
    for (unsigned i = 0; i < count; i++) {
        WgProbeEscape input = {0};
        WgDiagnosticResult result = {0};
        DWORD bytes = 0;
        input.magic = WG_MAGIC; input.version = WG_ABI_VERSION;
        if (!DeviceIoControl(device, IOCTL_WG_PROBE, &input, sizeof input,
                             &result, sizeof result, &bytes, NULL)) {
            fprintf(stderr, "Probe IOCTL failed: Win32=%lu\n", GetLastError()); return 1;
        }
        printf("[%u] status=0x%08lx state=%u error=%u fence=%llu gpu_ns=%llu\n", i,
            (unsigned long)result.ntstatus, result.probe.completion_state, result.probe.error,
            (unsigned long long)result.probe.fence, (unsigned long long)result.probe.gpu_nanoseconds);
        if (bytes != sizeof result || result.ntstatus || result.reserved ||
            result.probe.magic != WG_MAGIC || result.probe.version != WG_ABI_VERSION ||
            result.probe.completion_state != WG_COMPLETE || result.probe.error || result.probe.fence <= lastFence)
            return 1;
        lastFence = result.probe.fence;
    }
    puts("PASS: Windows kernel -> PCI batch -> Metal completion. Direct3D is not implemented.");
    return 0;
}
int wmain(int argc, wchar_t **argv) {
    unsigned count = 3;
    if (argc == 3 && !wcscmp(argv[1], L"--repeat")) {
        wchar_t *end;
        unsigned long value = wcstoul(argv[2], &end, 10);
        if (!*argv[2] || *end || !value || value > 1000) return 2;
        count = (unsigned)value;
    } else if (argc != 1) { fputs("Usage: WinGpuProbe.exe [--repeat 1..1000]\n", stderr); return 2; }
    HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_WINIOS_GPU_PROBE, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return 2;
    SP_DEVICE_INTERFACE_DATA entry = {0}; entry.cbSize = sizeof entry;
    if (!SetupDiEnumDeviceInterfaces(info, NULL, &GUID_DEVINTERFACE_WINIOS_GPU_PROBE, 0, &entry)) {
        fputs("Diagnostic device not found. Enable the experimental PCI device and load its driver first.\n", stderr);
        SetupDiDestroyDeviceInfoList(info); return 2;
    }
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(info, &entry, NULL, 0, &needed, NULL);
    if (!needed || GetLastError() != ERROR_INSUFFICIENT_BUFFER) { SetupDiDestroyDeviceInfoList(info); return 2; }
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = calloc(1, needed);
    if (!detail) { SetupDiDestroyDeviceInfoList(info); return 2; }
    detail->cbSize = sizeof *detail;
    HANDLE device = INVALID_HANDLE_VALUE;
    if (SetupDiGetDeviceInterfaceDetailW(info, &entry, detail, needed, NULL, NULL))
        device = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    DWORD error = GetLastError();
    free(detail);
    SetupDiDestroyDeviceInfoList(info);
    if (device == INVALID_HANDLE_VALUE) { fprintf(stderr, "Open diagnostic device: Win32=%lu (run as administrator).\n", error); return 2; }
    int result = run(device, count);
    CloseHandle(device);
    return result;
}
