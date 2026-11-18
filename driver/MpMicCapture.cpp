// MicPatch PCM producer interface. All FIFO and reader state is nonpaged.
#include "MpMicCapture.h"

namespace {
constexpr ULONG kTag = 'cMpM';
constexpr ULONG kCapacity = MPMIC_RATE / 10;
constexpr ULONGLONG kMaxAge = 100 * 10000ULL; // interrupt time, in 100 ns units
constexpr GUID kInterface = MPMIC_INTERFACE_GUID;
UNICODE_STRING kReference = RTL_CONSTANT_STRING(L"MicPatchPcm");
const UNICODE_STRING kOpenPath = RTL_CONSTANT_STRING(L"\\MicPatchPcm");
LIST_ENTRY gCaptures;
KSPIN_LOCK gListLock;
PDRIVER_DISPATCH gPortDispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];
UCHAR gControlFile;
}

struct MpMicCapture {
    LIST_ENTRY link;
    LONG references;
    PDEVICE_OBJECT device;
    UNICODE_STRING interfacePath; // PnP thread only
    KSPIN_LOCK lock;
    bool started;
    PFILE_OBJECT writer;
    const void* reader;
    ULONG head;
    ULONG count;
    short pcm[kCapacity];
    ULONGLONG written[kCapacity];
};

namespace {
MpMicCapture* FindCapture(PDEVICE_OBJECT device)
{
    for (auto link = gCaptures.Flink; link != &gCaptures; link = link->Flink) {
        auto capture = CONTAINING_RECORD(link, MpMicCapture, link);
        if (capture->device == device)
            return capture;
    }
    return nullptr;
}

void Clear(MpMicCapture* c)
{
    c->head = c->count = 0;
}

void Drop(MpMicCapture* c, ULONG frames)
{
    c->head = (c->head + frames) % kCapacity;
    c->count -= frames;
}

void Expire(MpMicCapture* c, ULONGLONG now)
{
    ULONG expired = 0;
    while (expired < c->count && now - c->written[(c->head + expired) % kCapacity] >= kMaxAge)
        ++expired;
    Drop(c, expired);
}

NTSTATUS DispatchControl(PDEVICE_OBJECT device, PIRP irp)
{
    const auto stack = IoGetCurrentIrpStackLocation(irp);
    const auto file = stack->FileObject;
    const auto major = stack->MajorFunction;
    const bool create = major == IRP_MJ_CREATE;
    const bool control = file && (create ? RtlEqualUnicodeString(&file->FileName, &kOpenPath, TRUE) != FALSE
                                          : file->FsContext2 == &gControlFile);
    if (!control)
        return gPortDispatch[major](device, irp);

    ULONG_PTR information = 0;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (create && (file->RelatedFileObject || !stack->Parameters.Create.SecurityContext ||
        !(stack->Parameters.Create.SecurityContext->DesiredAccess & (FILE_WRITE_DATA | GENERIC_WRITE)))) {
        status = STATUS_ACCESS_DENIED;
    } else if (major == IRP_MJ_CLOSE) {
        file->FsContext2 = nullptr;
        status = STATUS_SUCCESS;
    } else if (major != IRP_MJ_CREATE && major != IRP_MJ_CLEANUP && major != IRP_MJ_DEVICE_CONTROL) {
        status = STATUS_INVALID_DEVICE_REQUEST;
    } else if (auto c = MpMicReference(device)) {
        KIRQL irql;
        KeAcquireSpinLock(&c->lock, &irql);
        if (major == IRP_MJ_CLEANUP) {
            if (file == c->writer) {
                c->writer = nullptr;
                Clear(c);
            }
            status = STATUS_SUCCESS;
        } else if (c->started && create) {
            if (c->writer) {
                status = STATUS_SHARING_VIOLATION;
            } else {
                file->FsContext2 = &gControlFile;
                c->writer = file;
                Clear(c);
                status = STATUS_SUCCESS;
            }
        } else if (c->started && major == IRP_MJ_DEVICE_CONTROL && file == c->writer) {
            const auto io = stack->Parameters.DeviceIoControl;
            auto buffer = irp->AssociatedIrp.SystemBuffer;
            const ULONGLONG now = KeQueryInterruptTime();
            switch (io.IoControlCode) {
            case IOCTL_MPMIC_SUBMIT: {
                if (io.InputBufferLength != sizeof(MpMicSubmit) || io.OutputBufferLength || !buffer) {
                    status = STATUS_INVALID_BUFFER_SIZE;
                    break;
                }
                auto data = static_cast<const MpMicSubmit*>(buffer);
                if (data->version != MPMIC_VERSION || !data->frames || data->frames > MPMIC_BLOCK_FRAMES) {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                status = STATUS_SUCCESS;
                // A recording can stop between a QUERY and its corresponding SUBMIT.
                if (!c->reader)
                    break;
                Expire(c, now);
                if (data->frames > kCapacity - c->count)
                    Drop(c, data->frames - (kCapacity - c->count));
                ULONG tail = (c->head + c->count) % kCapacity;
                for (ULONG i = 0; i < data->frames; ++i) {
                    c->pcm[tail] = data->samples[i];
                    c->written[tail] = now;
                    tail = (tail + 1) % kCapacity;
                }
                c->count += data->frames;
                break;
            }
            case IOCTL_MPMIC_FLUSH:
                if (io.InputBufferLength || io.OutputBufferLength) {
                    status = STATUS_INVALID_BUFFER_SIZE;
                    break;
                }
                Clear(c);
                status = STATUS_SUCCESS;
                break;
            case IOCTL_MPMIC_QUERY:
                if (io.InputBufferLength || io.OutputBufferLength < sizeof(MpMicState) || !buffer) {
                    status = STATUS_INVALID_BUFFER_SIZE;
                    break;
                }
                Expire(c, now);
                *static_cast<MpMicState*>(buffer) = {MPMIC_VERSION, c->count, c->reader ? 1u : 0u, 0u};
                information = sizeof(MpMicState);
                status = STATUS_SUCCESS;
                break;
            default:
                status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }
        }
        KeReleaseSpinLock(&c->lock, irql);
        MpMicRelease(c);
    } else if (major == IRP_MJ_CLEANUP) {
        status = STATUS_SUCCESS;
    }
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}
}

void MpMicInitialize(PDRIVER_OBJECT driver)
{
    InitializeListHead(&gCaptures);
    KeInitializeSpinLock(&gListLock);
    const UCHAR majors[] = {IRP_MJ_CREATE, IRP_MJ_CLEANUP, IRP_MJ_CLOSE, IRP_MJ_DEVICE_CONTROL,
                            IRP_MJ_READ, IRP_MJ_WRITE, IRP_MJ_INTERNAL_DEVICE_CONTROL,
                            IRP_MJ_FLUSH_BUFFERS, IRP_MJ_QUERY_INFORMATION, IRP_MJ_SET_INFORMATION,
                            IRP_MJ_QUERY_SECURITY, IRP_MJ_SET_SECURITY};
    for (auto major : majors) {
        gPortDispatch[major] = driver->MajorFunction[major];
        driver->MajorFunction[major] = DispatchControl;
    }
}

NTSTATUS MpMicPrepare(PDEVICE_OBJECT device, PDEVICE_OBJECT pdo)
{
    PAGED_CODE();
    auto c = static_cast<MpMicCapture*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(MpMicCapture), kTag));
    if (!c)
        return STATUS_INSUFFICIENT_RESOURCES;
    c->device = device;
    c->references = 1;
    KeInitializeSpinLock(&c->lock);
    const NTSTATUS status = IoRegisterDeviceInterface(pdo, &kInterface, &kReference, &c->interfacePath);
    if (!NT_SUCCESS(status)) {
        MpMicRelease(c);
        return status;
    }
    // Apply the INF's device ACL to relative opens (including the producer path).
    device->Characteristics |= FILE_DEVICE_SECURE_OPEN;
    KIRQL irql;
    KeAcquireSpinLock(&gListLock, &irql);
    InsertTailList(&gCaptures, &c->link);
    KeReleaseSpinLock(&gListLock, irql);
    return STATUS_SUCCESS;
}

NTSTATUS MpMicEnable(PDEVICE_OBJECT device)
{
    PAGED_CODE();
    auto c = MpMicReference(device);
    if (!c)
        return STATUS_DEVICE_NOT_READY;
    KIRQL irql;
    KeAcquireSpinLock(&c->lock, &irql);
    c->started = true;
    KeReleaseSpinLock(&c->lock, irql);
    const NTSTATUS status = IoSetDeviceInterfaceState(&c->interfacePath, TRUE);
    MpMicRelease(c);
    return status;
}

void MpMicStop(PDEVICE_OBJECT device)
{
    PAGED_CODE();
    KIRQL irql;
    KeAcquireSpinLock(&gListLock, &irql);
    auto c = FindCapture(device);
    if (c) {
        // Lock order: list -> capture. No capture-lock holder takes the list lock.
        KeAcquireSpinLockAtDpcLevel(&c->lock);
        c->started = false;
        c->reader = nullptr;
        c->writer = nullptr;
        Clear(c);
        RemoveEntryList(&c->link);
        KeReleaseSpinLockFromDpcLevel(&c->lock);
    }
    KeReleaseSpinLock(&gListLock, irql);
    if (c) {
        IoSetDeviceInterfaceState(&c->interfacePath, FALSE);
        RtlFreeUnicodeString(&c->interfacePath);
        MpMicRelease(c);
    }
}

MpMicCapture* MpMicReference(PDEVICE_OBJECT device)
{
    KIRQL irql;
    KeAcquireSpinLock(&gListLock, &irql);
    auto c = FindCapture(device);
    if (c)
        InterlockedIncrement(&c->references);
    KeReleaseSpinLock(&gListLock, irql);
    return c;
}

void MpMicRelease(MpMicCapture* c)
{
    if (c && InterlockedDecrement(&c->references) == 0)
        ExFreePoolWithTag(c, kTag);
}

void MpMicSetRunning(MpMicCapture* c, const void* reader, bool running)
{
    if (!c)
        return;
    KIRQL irql;
    KeAcquireSpinLock(&c->lock, &irql);
    if (running && c->started && !c->reader) {
        c->reader = reader;
        Clear(c);
    } else if (!running && c->reader == reader) {
        c->reader = nullptr;
        Clear(c);
    }
    KeReleaseSpinLock(&c->lock, irql);
}

void MpMicRead(MpMicCapture* c, const void* reader, short* samples, ULONG frames)
{
    ULONG copied = 0;
    if (c) {
        KIRQL irql;
        KeAcquireSpinLock(&c->lock, &irql);
        if (c->started && c->reader == reader && c->writer) {
            Expire(c, KeQueryInterruptTime());
            copied = min(frames, c->count);
            const ULONG first = min(copied, kCapacity - c->head);
            RtlCopyMemory(samples, c->pcm + c->head, first * sizeof(short));
            if (first < copied)
                RtlCopyMemory(samples + first, c->pcm, (copied - first) * sizeof(short));
            Drop(c, copied);
        }
        KeReleaseSpinLock(&c->lock, irql);
    }
    if (copied < frames)
        RtlZeroMemory(samples + copied, (frames - copied) * sizeof(short));
}
