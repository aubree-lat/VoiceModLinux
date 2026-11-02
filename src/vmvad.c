/*
 * vmvad.sys - stand-in for Voicemod's virtual audio driver ("voicemodvad").
 *
 * Voicemod opens \\.\voicemodvad at startup and talks to it with a handful of
 * IOCTLs; if the device is missing its audio init never completes and the UI
 * stays black. This driver only answers those IOCTLs: no audio goes through
 * it. The real audio endpoints are PipeWire devices created by vmwine.
 *
 * Protocol (observed for interoperability, see README):
 *   0x1d2007  register an event (HANDLE as METHOD_NEITHER input, len >= 8)
 *   0x1d200b  unregister the event
 *   0x1d200f  signal the event
 *   0x1d2013  clear the event
 *   0x1d201c  xrun counters: 0x410-byte output buffer; first qword == 1 means reset
 */
#include <ddk/wdm.h>

#define XRUN_BUFFER_SIZE 0x410

static UNICODE_STRING dev_name  = RTL_CONSTANT_STRING(L"\\Device\\voicemodvad");
static UNICODE_STRING link_name = RTL_CONSTANT_STRING(L"\\DosDevices\\voicemodvad");

static NTSTATUS complete(PIRP irp, NTSTATUS status, ULONG_PTR info)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS WINAPI dispatch_create_close(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS WINAPI dispatch_ioctl(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG out_len = sp->Parameters.DeviceIoControl.OutputBufferLength;
    ULONGLONG *buf = irp->AssociatedIrp.SystemBuffer;
    (void)dev;

    switch (code) {
    case 0x1d2007:
    case 0x1d200b:
    case 0x1d200f:
    case 0x1d2013:
        return complete(irp, STATUS_SUCCESS, 0);

    case 0x1d201c:
        if (out_len != XRUN_BUFFER_SIZE || !buf)
            return complete(irp, STATUS_INVALID_PARAMETER, 0);
        if (buf[0] == 1)                /* reset request */
            return complete(irp, STATUS_SUCCESS, 0);
        RtlZeroMemory(buf, XRUN_BUFFER_SIZE);
        return complete(irp, STATUS_SUCCESS, XRUN_BUFFER_SIZE);

    default:
        return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static void WINAPI driver_unload(PDRIVER_OBJECT drv)
{
    IoDeleteSymbolicLink(&link_name);
    if (drv->DeviceObject) IoDeleteDevice(drv->DeviceObject);
}

NTSTATUS WINAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    PDEVICE_OBJECT dev;
    NTSTATUS status;
    (void)reg;

    status = IoCreateDevice(drv, 0, &dev_name, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
    if (!NT_SUCCESS(status)) return status;
    status = IoCreateSymbolicLink(&link_name, &dev_name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(dev);
        return status;
    }
    drv->MajorFunction[IRP_MJ_CREATE] = dispatch_create_close;
    drv->MajorFunction[IRP_MJ_CLOSE] = dispatch_create_close;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_ioctl;
    drv->DriverUnload = driver_unload;
    return STATUS_SUCCESS;
}
