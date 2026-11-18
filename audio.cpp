#include "audio.h"
#include "language.h"
#include <winioctl.h>
#include "virtual_mic_protocol.h"
#include <cfgmgr32.h>

#include <audioclient.h>
#include <avrt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <devpkey.h>
#include <cwchar>
#include <wrl/client.h>
#include <ksmedia.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace micpatch {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto conversion = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

void check(HRESULT hr) {
    if (FAILED(hr)) throw hr;
}

struct Com {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Com() { if (result != RPC_E_CHANGED_MODE) check(result); }
    ~Com() { if (SUCCEEDED(result)) CoUninitialize(); }
};

struct Mf {
    Mf() { check(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
    ~Mf() { MFShutdown(); }
};

struct Event {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Event() { if (!handle) throw HRESULT_FROM_WIN32(GetLastError()); }
    ~Event() { CloseHandle(handle); }
};

struct AudioPriority {
    DWORD index{};
    HANDLE handle = AvSetMmThreadCharacteristicsW(L"Audio", &index);
    ~AudioPriority() { if (handle) AvRevertMmThreadCharacteristics(handle); }
};

ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> result;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&result)));
    return result;
}

ComPtr<IAudioClient> client(const std::wstring& id) {
    ComPtr<IMMDevice> device;
    check(enumerator()->GetDevice(id.c_str(), &device));
    ComPtr<IAudioClient> result;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(result.GetAddressOf())));
    return result;
}

WAVEFORMATEX monoFormat() {
    return {WAVE_FORMAT_IEEE_FLOAT, 1, rate, rate * sizeof(float), sizeof(float), 32, 0};
}

bool waitAudio(HANDLE stop, HANDLE stream) {
    const HANDLE handles[]{stop, stream};
    switch (WaitForMultipleObjects(2, handles, FALSE, 3000)) {
    case WAIT_OBJECT_0: return false;
    case WAIT_OBJECT_0 + 1: return true;
    case WAIT_TIMEOUT: throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    default: throw HRESULT_FROM_WIN32(GetLastError());
    }
}

struct Started {
    IAudioClient* audio;
    explicit Started(IAudioClient* value) : audio(value) { check(audio->Start()); }
    ~Started() { audio->Stop(); }
};

GUID pcmInterface = MPMIC_INTERFACE_GUID;

std::wstring virtualPath() {
    ULONG size{};
    if (CM_Get_Device_Interface_List_SizeW(&size, &pcmInterface, nullptr,
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || size < 2) return {};
    std::vector<wchar_t> list(size);
    if (CM_Get_Device_Interface_ListW(&pcmInterface, nullptr, list.data(), size,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return {};
    std::wstring path(list.data());
    if (path.empty() || path.size() + 1 >= list.size() || list[path.size() + 1]) return {};
    return path;
}

bool isVirtualMic(IPropertyStore* properties, const std::wstring* expected = nullptr) {
    PROPVARIANT instance{};
    DEVINST node{};
    bool own{};
    if (SUCCEEDED(properties->GetValue(PKEY_Device_InstanceId, &instance)) &&
        instance.vt == VT_LPWSTR && instance.pwszVal &&
        CM_Locate_DevNodeW(&node, instance.pwszVal, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
        for (unsigned depth = 0; depth < 8 && !own; ++depth) {
            if (expected) {
                wchar_t actual[MAX_DEVICE_ID_LEN]{};
                if (CM_Get_Device_IDW(node, actual, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS &&
                    _wcsicmp(actual, expected->c_str()) == 0) { own = true; break; }
            }
            DEVPROPTYPE type{};
            wchar_t ids[1024]{};
            ULONG bytes = sizeof(ids);
            if (!expected && CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_HardwareIds, &type,
                    reinterpret_cast<PBYTE>(ids), &bytes, 0) == CR_SUCCESS &&
                type == DEVPROP_TYPE_STRING_LIST && bytes <= sizeof(ids)) {
                const auto* end = ids + bytes / sizeof(wchar_t);
                for (const wchar_t* start = ids; start < end && *start;) {
                    const auto* next = std::find(start, end, L'\0');
                    if (next == end) break;
                    if (_wcsicmp(start, L"ROOT\\MICPATCHMIC") == 0) { own = true; break; }
                    start = next + 1;
                }
            }
            DEVINST parent{};
            if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
            node = parent;
        }
    }
    PropVariantClear(&instance);
    return own;
}

struct WinHandle {
    HANDLE value;
    WinHandle(const WinHandle&) = delete;
    explicit WinHandle(HANDLE handle) : value(handle) {
        if (!value || value == INVALID_HANDLE_VALUE) throw HRESULT_FROM_WIN32(GetLastError());
    }
    ~WinHandle() { CloseHandle(value); }
};

class VirtualWriter {
    WinHandle device_;
    WinHandle completed_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE stop_;

    void request(DWORD code, void* input, DWORD inputSize, void* output, DWORD outputSize) {
        OVERLAPPED op{};
        op.hEvent = completed_.value;
        ResetEvent(op.hEvent);
        DWORD transferred{};
        if (!DeviceIoControl(device_.value, code, input, inputSize, output, outputSize, &transferred, &op)) {
            const auto error = GetLastError();
            if (error != ERROR_IO_PENDING) throw HRESULT_FROM_WIN32(error);
            const HANDLE events[]{stop_, op.hEvent};
            const auto result = WaitForMultipleObjects(2, events, FALSE, 500);
            if (result != WAIT_OBJECT_0 + 1) {
                const auto reason = result == WAIT_FAILED ? GetLastError() :
                    (result == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_CANCELLED);
                CancelIoEx(device_.value, &op);
                GetOverlappedResult(device_.value, &op, &transferred, TRUE);
                throw HRESULT_FROM_WIN32(reason);
            }
            if (!GetOverlappedResult(device_.value, &op, &transferred, FALSE))
                throw HRESULT_FROM_WIN32(GetLastError());
        }
        if (transferred != outputSize) throw HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

public:
    VirtualWriter(const std::wstring& path, HANDLE stop) :
        device_(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)),
        stop_(stop) {}

    MpMicState query() {
        MpMicState state{};
        request(IOCTL_MPMIC_QUERY, nullptr, 0, &state, sizeof(state));
        if (state.version != MPMIC_VERSION || state.active > 1 || state.reserved || state.queuedFrames > MPMIC_RATE)
            throw HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        return state;
    }
    void flush() { request(IOCTL_MPMIC_FLUSH, nullptr, 0, nullptr, 0); }
    void submit(MpMicSubmit& block) { request(IOCTL_MPMIC_SUBMIT, &block, sizeof(block), nullptr, 0); }
};

void seekStart(IMFSourceReader* reader) {
    PROPVARIANT position{};
    position.vt = VT_I8;
    check(reader->SetCurrentPosition(GUID_NULL, position));
}
}

std::vector<Endpoint> endpoints(bool capture) {
    std::vector<Endpoint> result;
    try {
        Com com;
        ComPtr<IMMDeviceCollection> collection;
        check(enumerator()->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection));
        UINT count{};
        check(collection->GetCount(&count));
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            ComPtr<IPropertyStore> properties;
            if (FAILED(collection->Item(i, &device)) || FAILED(device->OpenPropertyStore(STGM_READ, &properties))) continue;
            LPWSTR id{};
            if (FAILED(device->GetId(&id))) continue;
            PROPVARIANT value{};
            const auto hr = properties->GetValue(PKEY_Device_FriendlyName, &value);
            result.push_back({id, SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal ? value.pwszVal : id});
            PropVariantClear(&value);
            CoTaskMemFree(id);
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    } catch (HRESULT) {
        return {};
    }
    if (!capture && !virtualPath().empty())
        result.insert(result.begin(), {virtualMicId, tr(L"麦克风（声搭） · 直接输出", L"MicPatch Microphone · Direct output")});
    return result;
}

HRESULT virtualCaptureName(const std::wstring& instance, std::wstring& name) {
    name.clear();
    try {
        Com com;
        ComPtr<IMMDeviceCollection> collection;
        check(enumerator()->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection));
        UINT count{};
        check(collection->GetCount(&count));
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            ComPtr<IPropertyStore> properties;
            if (FAILED(collection->Item(i, &device)) || FAILED(device->OpenPropertyStore(STGM_READ, &properties))) continue;
            if (!isVirtualMic(properties.Get(), &instance)) continue;
            PROPVARIANT value{};
            const auto hr = properties->GetValue(PKEY_Device_FriendlyName, &value);
            if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal) name = value.pwszVal;
            PropVariantClear(&value);
            if (name.empty()) name = tr(L"MicPatch 虚拟麦克风", L"MicPatch Virtual Microphone");
            return S_OK;
        }
        return S_FALSE;
    } catch (HRESULT hr) {
        return hr;
    }
}

Engine::~Engine() { stop(); }

std::wstring Engine::start(const std::wstring& output, const std::wstring& mic, const std::wstring& file) {
    if (active_) return tr(L"音频引擎已在运行。", L"The audio engine is already running.");
    stop();
    if (output.empty() || (mic.empty() && file.empty())) return tr(L"请选择输出设备并添加至少一个音源。", L"Select an output and add at least one source.");
    if (output == virtualMicId && !mic.empty()) {
        try {
            Com com;
            ComPtr<IMMDevice> device;
            ComPtr<IPropertyStore> properties;
            check(enumerator()->GetDevice(mic.c_str(), &device));
            check(device->OpenPropertyStore(STGM_READ, &properties));
            if (isVirtualMic(properties.Get())) return tr(L"不能把麦克风（声搭）同时设为音源和输出。", L"MicPatch Microphone cannot be both the source and the output.");
        } catch (HRESULT) {
            return tr(L"无法验证麦克风音源，请刷新设备后重试。", L"Could not verify the microphone. Refresh the device list and try again.");
        }
    }
    {
        std::scoped_lock lock(errorMutex_);
        error_.clear();
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return tr(L"无法创建音频线程停止事件。", L"Could not create the audio stop event.");
    micLost_ = fileLost_ = false;
    micEpoch_ = 0;
    active_ = true;
    try {
        if (!mic.empty()) captureThread_ = std::thread(&Engine::capture, this, mic);
        if (!file.empty()) fileThread_ = std::thread(&Engine::decode, this, file);
        renderThread_ = std::thread(&Engine::render, this, output, !mic.empty(), !file.empty());
    } catch (const std::exception&) {
        stop();
        return tr(L"无法创建音频线程。", L"Could not start the audio threads.");
    }
    return {};
}

void Engine::stop() {
    if (stopEvent_) SetEvent(stopEvent_);
    if (renderThread_.joinable()) renderThread_.join();
    if (captureThread_.joinable()) captureThread_.join();
    if (fileThread_.joinable()) fileThread_.join();
    if (stopEvent_) CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
    ready_ = false;
    active_ = false;
    micPeak = filePeak = outPeak = 0;
    micQueue_.reset();
    fileQueue_.reset();
}

void Engine::fail(const wchar_t* context, HRESULT code) {
    wchar_t hex[20]{};
    swprintf_s(hex, L"0x%08lX", static_cast<unsigned long>(code));
    std::scoped_lock lock(errorMutex_);
    if (!error_.empty()) error_ += L"；";
    error_ += std::wstring(context) + L"（" + hex + L"）";
}

std::wstring Engine::error() {
    std::scoped_lock lock(errorMutex_);
    return error_;
}

void Engine::capture(std::wstring id) {
    try {
        Com com;
        Event event;
        auto audio = client(id);
        auto format = monoFormat();
        check(audio->Initialize(AUDCLNT_SHAREMODE_SHARED, conversion | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                200000, 0, &format, nullptr));
        check(audio->SetEventHandle(event.handle));
        ComPtr<IAudioCaptureClient> input;
        check(audio->GetService(IID_PPV_ARGS(&input)));
        AudioPriority priority;
        Started started(audio.Get());
        bool firstPacket = true;
        while (waitAudio(stopEvent_, event.handle)) {
            for (;;) {
                UINT32 pending{};
                check(input->GetNextPacketSize(&pending));
                if (!pending) break;
                BYTE* data{};
                UINT32 frames{};
                DWORD flags{};
                check(input->GetBuffer(&data, &frames, &flags, nullptr, nullptr));
                if (!firstPacket && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) ++micEpoch_;
                firstPacket = false;
                const auto samples = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < frames; ++i) {
                    if (!micQueue_.push(flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0 : samples[i])) {
                        ++micEpoch_;
                        break;
                    }
                }
                check(input->ReleaseBuffer(frames));
                if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) return;
            }
        }
    } catch (HRESULT hr) {
        micLost_ = true;
        fail(tr(L"麦克风采集已停止，请检查设备后重新启动", L"Microphone capture stopped. Check the device and restart."), hr);
    }
}

struct Engine::MixState {
    ClockFollower clock;
    VoiceEq eq;
    Ducker ducker;
    PeakLimiter limiter;
    unsigned epoch{};
};

void Engine::mixBlock(MixState& s, float* dest, unsigned frames, bool hasMic, bool hasFile, bool muted) {
    const bool useMic = hasMic && !micLost_.load();
    const bool useFile = hasFile && playing.load() && !fileLost_.load();
    const auto epoch = micEpoch_.load();
    if (epoch != s.epoch) {
        micQueue_.consume(micQueue_.size());
        s.clock = {};
        s.eq.resetSignal();
        s.epoch = epoch;
    }
    const auto mg = micGain.load(), fg = fileGain.load();
    const auto mm = micMute.load(), fm = fileMute.load();
    const auto useEq = voiceEq.load(), useDuck = ducking.load() && useMic && !mm;
    float mp{}, fp{}, op{};
    for (unsigned i = 0; i < frames; ++i) {
        const auto sample = useMic ? s.clock.next(micQueue_) : 0.0f;
        const bool micReady = useMic && s.clock.ready();
        if (!micReady) s.eq.resetSignal();
        const auto mic = scaled(s.eq.next(std::isfinite(sample) ? sample : 0.0f, useEq), mg, mm);
        float decoded{};
        if (useFile) fileQueue_.pop(decoded);
        const auto file = scaled(decoded, fg, fm) * s.ducker.next(mic, useDuck && micReady);
        const auto mixed = s.limiter.next(mic + file);
        dest[i] = muted ? 0 : mixed;
        mp = std::max(mp, std::abs(mic));
        fp = std::max(fp, std::abs(file));
        op = std::max(op, std::abs(dest[i]));
    }
    micPeak = mp;
    filePeak = fp;
    outPeak = op;
}

void Engine::renderVirtual(bool hasMic, bool hasFile) {
    auto path = virtualPath();
    if (path.empty()) throw HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    VirtualWriter writer(path, stopEvent_);
    WinHandle timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_MODIFY_STATE | SYNCHRONIZE));
    LARGE_INTEGER due{};
    due.QuadPart = -1;
    if (!SetWaitableTimer(timer.value, &due, 5, nullptr, nullptr, FALSE))
        throw HRESULT_FROM_WIN32(GetLastError());
    AudioPriority priority;
    MixState mix;
    constexpr unsigned target = MPMIC_BLOCK_FRAMES * 2;
    std::array<float, MPMIC_BLOCK_FRAMES> audio{};
    MpMicSubmit block{};
    block.version = MPMIC_VERSION;
    bool wasMuted{};
    while (waitAudio(stopEvent_, timer.value)) {
        const bool muted = outMute.load();
        if (muted && !wasMuted) writer.flush();
        wasMuted = muted;
        const auto state = writer.query();
        ready_ = true;
        const auto frames = !state.active ? MPMIC_BLOCK_FRAMES / 2 :
            std::min(MPMIC_BLOCK_FRAMES, target - std::min(target, state.queuedFrames));
        if (!frames) continue;
        mixBlock(mix, audio.data(), frames, hasMic, hasFile, muted);
        if (state.active) {
            if (!muted && outMute.load()) {
                writer.flush();
                wasMuted = true;
                outPeak = 0;
                continue;
            }
            block.frames = frames;
            for (unsigned i = 0; i < frames; ++i)
                block.samples[i] = virtualPcm16(audio[i]);
            writer.submit(block);
        } else {
            outPeak = 0;
        }
    }
}

void Engine::render(std::wstring id, bool hasMic, bool hasFile) {
    const bool direct = id == virtualMicId;
    try {
        if (direct) {
            renderVirtual(hasMic, hasFile);
        } else {
            renderWasapi(id, hasMic, hasFile);
        }
    } catch (HRESULT hr) {
        if (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
            fail(direct ? tr(L"与麦克风（声搭）的连接已停止，请检查驱动、权限或其他写入程序", L"MicPatch Microphone disconnected. Check the driver, permissions, or other writers.")
                        : tr(L"输出已停止，请检查选中的播放设备", L"Output stopped. Check the selected playback device."), hr);
            SetEvent(stopEvent_);
        }
    }
    micPeak = filePeak = outPeak = 0;
    ready_ = false;
    active_ = false;
}

void Engine::renderWasapi(const std::wstring& id, bool hasMic, bool hasFile) {
    Com com;
    Event event;
    auto audio = client(id);
    auto format = monoFormat();
    check(audio->Initialize(AUDCLNT_SHAREMODE_SHARED, conversion | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            200000, 0, &format, nullptr));
    check(audio->SetEventHandle(event.handle));
    UINT32 capacity{};
    check(audio->GetBufferSize(&capacity));
    ComPtr<IAudioRenderClient> output;
    check(audio->GetService(IID_PPV_ARGS(&output)));
    BYTE* data{};
    check(output->GetBuffer(capacity, &data));
    check(output->ReleaseBuffer(capacity, AUDCLNT_BUFFERFLAGS_SILENT));
    AudioPriority priority;
    Started started(audio.Get());
    ready_ = true;
    MixState mix;
    while (waitAudio(stopEvent_, event.handle)) {
        UINT32 padding{};
        check(audio->GetCurrentPadding(&padding));
        const auto frames = capacity - padding;
        if (!frames) continue;
        check(output->GetBuffer(frames, &data));
        auto dest = reinterpret_cast<float*>(data);
        mixBlock(mix, dest, frames, hasMic, hasFile, outMute.load());
        check(output->ReleaseBuffer(frames, outPeak.load() == 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0));
    }
}

void Engine::decode(std::wstring path) {
    try {
        Com com;
        Mf mf;
        ComPtr<IMFSourceReader> reader;
        const auto stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        check(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader));
        check(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
        check(reader->SetStreamSelection(stream, TRUE));
        ComPtr<IMFMediaType> requested;
        check(MFCreateMediaType(&requested));
        check(requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        const auto setSubtype = [&](const GUID& subtype) {
            check(requested->SetGUID(MF_MT_SUBTYPE, subtype));
            return reader->SetCurrentMediaType(stream, nullptr, requested.Get());
        };
        if (FAILED(setSubtype(MFAudioFormat_Float))) check(setSubtype(MFAudioFormat_PCM));
        UINT32 channels{}, sampleRate{}, alignment{}, bits{};
        bool isFloat{};
        const auto readFormat = [&] {
            ComPtr<IMFMediaType> actual;
            check(reader->GetCurrentMediaType(stream, &actual));
            GUID subtype{};
            check(actual->GetGUID(MF_MT_SUBTYPE, &subtype));
            check(actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels));
            check(actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate));
            check(actual->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &alignment));
            check(actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits));
            isFloat = subtype == MFAudioFormat_Float;
            if ((!isFloat && subtype != MFAudioFormat_PCM) || (isFloat && bits != 32) ||
                (bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
                !channels || channels > 32 || sampleRate < 8000 || sampleRate > 384000 ||
                alignment != channels * (bits / 8)) throw MF_E_INVALIDMEDIATYPE;
        };
        readFormat();
        constexpr auto stopped = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        const auto enqueue = [&](float value) {
            while (!fileQueue_.push(value)) {
                if (WaitForSingleObject(stopEvent_, 4) == WAIT_OBJECT_0) throw stopped;
            }
        };
        unsigned phase{};
        float previous{};
        bool havePrevious{};
        const auto convert = [&](float value) {
            if (havePrevious) {
                while (phase < rate) {
                    enqueue(previous + (value - previous) * (float(phase) / rate));
                    phase += sampleRate;
                }
                phase -= rate;
            }
            previous = value;
            havePrevious = true;
        };
        bool eof{}, restart{}, hadSamples{};
        while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
            if (!playing || fileQueue_.size() >= 8192 || (eof && !loop && fileQueue_.size() != 0)) {
                WaitForSingleObject(stopEvent_, 4);
                continue;
            }
            if (eof) {
                if (!hadSamples) throw MF_E_INVALIDMEDIATYPE;
                if (!loop) {
                    playing = false;
                    restart = true;
                    eof = false;
                    continue;
                }
                restart = true;
            }
            if (restart) {
                seekStart(reader.Get());
                restart = eof = hadSamples = havePrevious = false;
                phase = 0;
            }
            DWORD flags{};
            ComPtr<IMFSample> sample;
            check(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample));
            if (flags & MF_SOURCE_READERF_ERROR) throw E_FAIL;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                readFormat();
                havePrevious = false;
                phase = 0;
            }
            if (sample) {
                ComPtr<IMFMediaBuffer> buffer;
                check(sample->ConvertToContiguousBuffer(&buffer));
                BYTE* data{};
                DWORD bytes{};
                check(buffer->Lock(&data, nullptr, &bytes));
                try {
                    const auto stride = bits / 8;
                    for (DWORD frame = 0; frame < bytes / alignment; ++frame) {
                        float sum{};
                        for (UINT32 ch = 0; ch < channels; ++ch) {
                            const auto p = data + frame * alignment + ch * stride;
                            float v{};
                            if (isFloat) {
                                std::memcpy(&v, p, sizeof(v));
                            } else if (bits == 8) {
                                v = (int(*p) - 128) / 128.0f;
                            } else {
                                std::uint32_t word{};
                                std::memcpy(&word, p, stride);
                                v = static_cast<float>(static_cast<std::int32_t>(word << (32 - bits))) / 2147483648.0f;
                            }
                            if (std::isfinite(v)) sum += std::clamp(v, -1.0f, 1.0f);
                        }
                        convert(sum / float(channels));
                    }
                    hadSamples = hadSamples || bytes >= alignment;
                } catch (...) {
                    buffer->Unlock();
                    throw;
                }
                check(buffer->Unlock());
            }
            eof = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
            if (eof && havePrevious) convert(previous);
        }
    } catch (HRESULT hr) {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) return;
        fileLost_ = true;
        playing = false;
        fail(tr(L"文件读取失败，请检查文件格式或访问权限", L"Could not read the file. Check its format and permissions."), hr);
    }
}
}
