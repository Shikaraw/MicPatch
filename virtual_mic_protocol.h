#pragma once

#define MPMIC_VERSION 1u
#define MPMIC_RATE 48000u
#define MPMIC_BLOCK_FRAMES 480u
#define MPMIC_INTERFACE_GUID {0x4596f103, 0x3a60, 0x4aa1, {0x98, 0x5e, 0x77, 0x14, 0xdc, 0xf5, 0xae, 0x19}}

#define IOCTL_MPMIC_SUBMIT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_MPMIC_FLUSH CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_MPMIC_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_DATA)

typedef struct MpMicSubmit {
    unsigned int version;
    unsigned int frames;
    short samples[MPMIC_BLOCK_FRAMES];
} MpMicSubmit;

typedef struct MpMicState {
    unsigned int version;
    unsigned int queuedFrames;
    unsigned int active;
    unsigned int reserved;
} MpMicState;

#ifdef __cplusplus
static_assert(sizeof(MpMicSubmit) == 968);
static_assert(sizeof(MpMicState) == 16);
#endif
