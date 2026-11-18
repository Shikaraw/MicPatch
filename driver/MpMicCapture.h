#pragma once

#include <ntddk.h>
#include "virtual_mic_protocol.h"

struct MpMicCapture;

void MpMicInitialize(PDRIVER_OBJECT driver);
NTSTATUS MpMicPrepare(PDEVICE_OBJECT device, PDEVICE_OBJECT pdo);
NTSTATUS MpMicEnable(PDEVICE_OBJECT device);
void MpMicStop(PDEVICE_OBJECT device);
MpMicCapture* MpMicReference(PDEVICE_OBJECT device);
void MpMicRelease(MpMicCapture* capture);
void MpMicSetRunning(MpMicCapture* capture, const void* reader, bool running);
void MpMicRead(MpMicCapture* capture, const void* reader, short* samples, ULONG frames);
