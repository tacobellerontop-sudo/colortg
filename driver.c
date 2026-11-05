#ifndef _AMD64_
#define _AMD64_
#endif
#ifndef _WIN64
#define _WIN64
#endif

#include <ntddk.h>
#include <ntddmou.h>

#define IOCTL_MOUSE_CLICK CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MOUSE_DOWN  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MOUSE_UP    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _DEVICE_EXTENSION {
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT LowerDevice;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

static CONNECT_DATA g_MouseConnect = { 0 };
static KSPIN_LOCK g_Lock;
static PDEVICE_OBJECT g_ControlDevice = NULL;

VOID SendMousePacket(USHORT buttonFlags) {
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_Lock, &oldIrql);

    if (g_MouseConnect.ClassService && g_MouseConnect.ClassDeviceObject) {
        MOUSE_INPUT_DATA mid = { 0 };
        ULONG consumed = 0;
        mid.ButtonFlags = buttonFlags;

        ((PSERVICE_CALLBACK_ROUTINE)g_MouseConnect.ClassService)(
            g_MouseConnect.ClassDeviceObject,
            &mid,
            (&mid) + 1,
            &consumed);
    }

    KeReleaseSpinLock(&g_Lock, oldIrql);
}

NTSTATUS DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS DispatchPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_EXTENSION ext = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (DeviceObject == g_ControlDevice) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    if (stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        if (ext && ext->LowerDevice) {
            IoDetachDevice(ext->LowerDevice);
        }
        IoDeleteDevice(DeviceObject);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->LowerDevice, Irp);
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDevice, Irp);
}

NTSTATUS DispatchInternalDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_EXTENSION ext = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (DeviceObject != g_ControlDevice && stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_MOUSE_CONNECT) {
        if (stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(CONNECT_DATA)) {
            PCONNECT_DATA cd = (PCONNECT_DATA)stack->Parameters.DeviceIoControl.Type3InputBuffer;
            if (cd) {
                KIRQL oldIrql;
                KeAcquireSpinLock(&g_Lock, &oldIrql);
                g_MouseConnect = *cd;
                KeReleaseSpinLock(&g_Lock, oldIrql);
                // DO NOT mutate cd->ClassService: avoids dangling pointers on driver unload
            }
        }
    }

    if (DeviceObject != g_ControlDevice && ext && ext->LowerDevice) {
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->LowerDevice, Irp);
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS DispatchControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    if (DeviceObject == g_ControlDevice) {
        PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
        ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;

        if (code == IOCTL_MOUSE_DOWN) {
            SendMousePacket(MOUSE_LEFT_BUTTON_DOWN);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        } else if (code == IOCTL_MOUSE_UP) {
            SendMousePacket(MOUSE_LEFT_BUTTON_UP);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        } else if (code == IOCTL_MOUSE_CLICK) {
            SendMousePacket(MOUSE_LEFT_BUTTON_DOWN);
            KeStallExecutionProcessor(1500); // 1.5ms safe stall
            SendMousePacket(MOUSE_LEFT_BUTTON_UP);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        }
    }

    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
    PDEVICE_OBJECT filterDev = NULL;
    NTSTATUS status = IoCreateDevice(
        DriverObject,
        sizeof(DEVICE_EXTENSION),
        NULL,
        FILE_DEVICE_MOUSE,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &filterDev);

    if (!NT_SUCCESS(status)) return status;

    PDEVICE_EXTENSION ext = (PDEVICE_EXTENSION)filterDev->DeviceExtension;
    RtlZeroMemory(ext, sizeof(DEVICE_EXTENSION));
    ext->Self = filterDev;
    ext->LowerDevice = IoAttachDeviceToDeviceStack(filterDev, PhysicalDeviceObject);
    if (!ext->LowerDevice) {
        IoDeleteDevice(filterDev);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    filterDev->Flags |= (ext->LowerDevice->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE));
    filterDev->DeviceType = ext->LowerDevice->DeviceType;
    filterDev->Characteristics = ext->LowerDevice->Characteristics;
    filterDev->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

VOID DriverUnload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);

    UNICODE_STRING symName;
    RtlInitUnicodeString(&symName, L"\\DosDevices\\MouClicker");
    IoDeleteSymbolicLink(&symName);

    if (g_ControlDevice) {
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = NULL;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_Lock, &oldIrql);
    RtlZeroMemory(&g_MouseConnect, sizeof(CONNECT_DATA));
    KeReleaseSpinLock(&g_Lock, oldIrql);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    KeInitializeSpinLock(&g_Lock);

    DriverObject->DriverUnload = DriverUnload;
    DriverObject->DriverExtension->AddDevice = AddDevice;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = DispatchInternalDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchControl;

    UNICODE_STRING devName, symName;
    RtlInitUnicodeString(&devName, L"\\Device\\MouClicker");
    RtlInitUnicodeString(&symName, L"\\DosDevices\\MouClicker");

    NTSTATUS status = IoCreateDevice(
        DriverObject,
        0,
        &devName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_ControlDevice);

    if (NT_SUCCESS(status)) {
        status = IoCreateSymbolicLink(&symName, &devName);
        if (NT_SUCCESS(status)) {
            g_ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
        } else {
            IoDeleteDevice(g_ControlDevice);
            g_ControlDevice = NULL;
        }
    }

    return status;
}
