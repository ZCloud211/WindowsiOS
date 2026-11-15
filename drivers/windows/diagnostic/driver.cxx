/* SPDX-License-Identifier: MIT
 * PCI diagnostic function driver. Does not register any display adapter/DDI.
 */
#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include "../../../gpu/wingpu_ioctl.h"
#include "../common/transport.hxx"

struct DeviceContext { WgTransport Gpu; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, Context)
extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AddDevice;
EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
EVT_WDF_DEVICE_D0_ENTRY EnterD0;
EVT_WDF_DEVICE_D0_EXIT ExitD0;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Control;

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, AddDevice);
    return WdfDriverCreate(driver, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
NTSTATUS AddDevice(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(init, WG_DIAGNOSTIC_DEVICE_TYPE);
    WDF_PNPPOWER_EVENT_CALLBACKS callbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtDevicePrepareHardware = Prepare;
    callbacks.EvtDeviceReleaseHardware = Release;
    callbacks.EvtDeviceD0Entry = EnterD0;
    callbacks.EvtDeviceD0Exit = ExitD0;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &callbacks);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    WgTransportInitialize(&Context(device)->Gpu);
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_WINIOS_GPU_PROBE, NULL);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = Control;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}
NTSTATUS Prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(raw);
    WgTransport *gpu = &Context(device)->Gpu;
    for (ULONG i = 0; i < WdfCmResourceListGetCount(translated); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (r && r->Type == CmResourceTypeMemory && r->u.Memory.Length == 4096)
            return WgTransportMap(gpu, r);
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}
NTSTATUS Release(WDFDEVICE device, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(translated);
    WgTransportStop(&Context(device)->Gpu);
    return STATUS_SUCCESS;
}
NTSTATUS EnterD0(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);
    return WgTransportResume(&Context(device)->Gpu);
}
NTSTATUS ExitD0(WDFDEVICE device, WDF_POWER_DEVICE_STATE target) {
    UNREFERENCED_PARAMETER(target);
    return WgTransportPause(&Context(device)->Gpu);
}
VOID Control(WDFQUEUE queue, WDFREQUEST request, size_t outputBytes, size_t inputBytes, ULONG code) {
    if (code != IOCTL_WG_PROBE) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    if (inputBytes != sizeof(WgProbeEscape) || outputBytes != sizeof(WgDiagnosticResult)) {
        WdfRequestComplete(request, STATUS_INFO_LENGTH_MISMATCH);
        return;
    }
    WgProbeEscape *input;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(request, sizeof *input, reinterpret_cast<PVOID *>(&input), NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    if (input->magic != WG_MAGIC || input->version != WG_ABI_VERSION || input->error ||
        input->completion_state || input->fence || input->gpu_nanoseconds) {
        WdfRequestComplete(request, STATUS_INVALID_PARAMETER);
        return;
    }
    WgDiagnosticResult *result;
    status = WdfRequestRetrieveOutputBuffer(request, sizeof *result, reinterpret_cast<PVOID *>(&result), NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    RtlZeroMemory(result, sizeof *result);
    result->probe.magic = WG_MAGIC;
    result->probe.version = WG_ABI_VERSION;
    result->ntstatus = static_cast<ULONG>(WgTransportProbe(&Context(WdfIoQueueGetDevice(queue))->Gpu, &result->probe));
    /* Return transport errors in a fully initialized response for diagnosis. */
    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof *result);
}
