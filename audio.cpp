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

ComPtr<IMMDevice> deviceFor(const std::wstring& id) {
    ComPtr<IMMDevice> device;
    check(enumerator()->GetDevice(id.c_str(), &device));
    return device;
}

std::wstring endpointId(IMMDevice* device) {
    LPWSTR id{};
    check(device->GetId(&id));
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(id, CoTaskMemFree);
    return id;
}

bool sameEndpoint(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

ComPtr<IAudioClient> client(IMMDevice* device, EDataFlow expected) {
    ComPtr<IMMEndpoint> endpoint;
    check(device->QueryInterface(IID_PPV_ARGS(&endpoint)));
    EDataFlow flow{};
    check(endpoint->GetDataFlow(&flow));
    if (flow != expected) throw E_INVALIDARG;
    ComPtr<IAudioClient> result;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(result.GetAddressOf())));
    return result;
}

WAVEFORMATEX monoFormat() {
    return {WAVE_FORMAT_IEEE_FLOAT, 1, rate, rate * sizeof(float), sizeof(float), 32, 0};
}

struct SampleFormat {
    unsigned channels{}, sampleRate{}, alignment{}, bits{};
    bool floating{};

    void validate() const {
        if (!channels || channels > 32 || sampleRate < 8000 || sampleRate > 384000 ||
            (floating ? (bits != 32 && bits != 64) : (bits != 8 && bits != 16 && bits != 24 && bits != 32)) ||
            alignment != channels * (bits / 8)) throw MF_E_INVALIDMEDIATYPE;
    }

    float mono(const BYTE* frame) const {
        const auto stride = bits / 8;
        float sum{};
        for (unsigned ch = 0; ch < channels; ++ch, frame += stride) {
            float value{};
            if (floating && bits == 32) {
                std::memcpy(&value, frame, sizeof(value));
            } else if (floating) {
                double wide{};
                std::memcpy(&wide, frame, sizeof(wide));
                if (std::isfinite(wide)) value = static_cast<float>(std::clamp(wide, -1.0, 1.0));
            } else if (bits == 8) {
                value = (int(*frame) - 128) / 128.0f;
            } else {
                std::uint32_t word{};
                std::memcpy(&word, frame, stride);
                value = static_cast<float>(static_cast<std::int32_t>(word << (32 - bits))) / 2147483648.0f;
            }
            if (std::isfinite(value)) sum += std::clamp(value, -1.0f, 1.0f);
        }
        return sum / static_cast<float>(channels);
    }
};

SampleFormat mixFormat(const WAVEFORMATEX& format) {
    constexpr GUID pcm{DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_PCM)};
    constexpr GUID ieee{DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_IEEE_FLOAT)};
    bool floating = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool integer = format.wFormatTag == WAVE_FORMAT_PCM;
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        floating = IsEqualGUID(extended.SubFormat, ieee);
        integer = IsEqualGUID(extended.SubFormat, pcm);
    }
    if (!integer && !floating) throw AUDCLNT_E_UNSUPPORTED_FORMAT;
    SampleFormat decoded{format.nChannels, format.nSamplesPerSec, format.nBlockAlign,
                         format.wBitsPerSample, floating};
    decoded.validate();
    return decoded;
}

struct Resampler {
    unsigned phase{};
    float previous{};
    bool havePrevious{};

    template<class Emit>
    void next(float value, unsigned sampleRate, const Emit& emit) {
        if (havePrevious) {
            while (phase < rate) {
                emit(previous + (value - previous) * (static_cast<float>(phase) / rate));
                phase += sampleRate;
            }
            phase -= rate;
        }
        previous = value;
        havePrevious = true;
    }
};

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

struct Engine::Source {
    const SourceConfig config;
    SourceControls controls;
    Ring<8192> queue;
    std::atomic<unsigned> epoch{};
    std::thread worker;

    explicit Source(const SourceConfig& value) : config(value) {
        controls.gain = static_cast<float>(std::clamp(value.gain, 0, 200)) / 100.0f;
        controls.mute = value.mute;
        controls.loop = value.loop;
        controls.voiceEq = value.voiceEq;
        controls.ducking = value.ducking;
    }
};

Engine::Engine() = default;
Engine::~Engine() { stop(); }

std::wstring Engine::start(const std::wstring& output, const std::vector<SourceConfig>& sources) {
    if (active_) return tr(L"音频引擎已在运行。", L"The audio engine is already running.");
    stop();
    {
        std::scoped_lock lock(errorMutex_);
        outputError_.clear();
        for (auto& message : sourceErrors_) message.clear();
    }
    if (output.empty() || sources.empty())
        return tr(L"请选择输出设备并添加至少一个音源。", L"Select an output and add at least one source.");
    if (output.find(L'\0') != std::wstring::npos)
        return tr(L"输出设备标识无效。", L"Invalid output device ID.");
    if (sources.size() > maxSources)
        return tr(L"最多可添加 16 路音源。", L"At most 16 audio sources are supported.");

    const bool direct = sameEndpoint(output, virtualMicId);
    auto outputId = direct ? std::wstring(virtualMicId) : output;
    const auto collision = [] {
        return tr(L"不能回采当前的输出设备。", L"The selected output device cannot also be a loopback source.");
    };
    if (std::any_of(sources.begin(), sources.end(), [&](const auto& config) {
            return config.kind == SourceKind::Loopback && config.target.find(L'\0') == std::wstring::npos &&
                   sameEndpoint(config.target, outputId);
        })) return collision();

    // Missing devices are diagnosed in their own workers. Detect known feedback
    // paths before starting any streams, then check again when opening capture.
    const auto targetKind = direct ? SourceKind::Microphone : SourceKind::Loopback;
    const auto needsCheck = [&](const SourceConfig& config) {
        return config.kind == targetKind && !config.target.empty() &&
            config.target.find(L'\0') == std::wstring::npos;
    };
    if (std::any_of(sources.begin(), sources.end(), needsCheck)) {
        try {
            Com com;
            if (!direct) outputId = endpointId(deviceFor(outputId).Get());
            for (const auto& config : sources) {
                if (!needsCheck(config)) continue;
                try {
                    auto device = deviceFor(config.target);
                    if (!direct) {
                        if (sameEndpoint(endpointId(device.Get()), outputId)) return collision();
                    } else {
                        ComPtr<IPropertyStore> properties;
                        check(device->OpenPropertyStore(STGM_READ, &properties));
                        if (isVirtualMic(properties.Get()))
                            return tr(L"不能把麦克风（声搭）同时设为音源和输出。", L"MicPatch Microphone cannot be both the source and the output.");
                    }
                } catch (HRESULT) {}
            }
        } catch (HRESULT) {}
    }

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return tr(L"无法创建音频线程停止事件。", L"Could not create the audio stop event.");
    try {
        for (const auto& config : sources) {
            sources_[sourceCount_] = std::make_unique<Source>(config);
            ++sourceCount_;
        }
        active_ = true;
        for (std::size_t i = 0; i < sourceCount_; ++i) {
            auto& source = *sources_[i];
            if (source.config.target.empty() || source.config.target.find(L'\0') != std::wstring::npos) {
                failSource(i, tr(L"未选择有效音源目标", L"No valid source target selected"), E_INVALIDARG);
                continue;
            }
            try {
                source.worker = std::thread(&Engine::runSource, this, i, outputId);
            } catch (const std::system_error&) {
                failSource(i, tr(L"无法创建音源线程", L"Could not start source thread"), E_FAIL);
            }
        }
        renderThread_ = std::thread(&Engine::render, this, std::move(outputId));
    } catch (const std::exception&) {
        stop();
        return tr(L"无法创建音频线程。", L"Could not start the audio threads.");
    }
    return {};
}

SourceControls* Engine::source(std::size_t index) {
    return index < sourceCount_ ? &sources_[index]->controls : nullptr;
}

void Engine::stop() {
    if (stopEvent_) SetEvent(stopEvent_);
    if (renderThread_.joinable()) renderThread_.join();
    for (std::size_t i = 0; i < sourceCount_; ++i) {
        auto& slot = sources_[i];
        if (slot->worker.joinable()) slot->worker.join();
        slot.reset();
    }
    sourceCount_ = 0;
    if (stopEvent_) CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
    ready_ = false;
    active_ = false;
    outPeak = 0;
}

static std::wstring errorMessage(const wchar_t* context, HRESULT code) {
    wchar_t hex[20]{};
    swprintf_s(hex, L"0x%08lX", static_cast<unsigned long>(code));
    return std::wstring(context) + L"（" + hex + L"）";
}

void Engine::failSource(std::size_t index, const wchar_t* context, HRESULT code) {
    auto& controls = sources_[index]->controls;
    {
        std::scoped_lock lock(errorMutex_);
        sourceErrors_[index] = std::wstring(tr(L"音源 ", L"Source ")) + std::to_wstring(index + 1) +
            L": " + errorMessage(context, code);
    }
    controls.failed = true;
    controls.playing = false;
    controls.peak = 0;
}

void Engine::failOutput(const wchar_t* context, HRESULT code) {
    std::scoped_lock lock(errorMutex_);
    outputError_ = errorMessage(context, code);
}

std::wstring Engine::error() {
    std::scoped_lock lock(errorMutex_);
    auto result = outputError_;
    for (const auto& message : sourceErrors_) {
        if (message.empty()) continue;
        if (!result.empty()) result += L"；";
        result += message;
    }
    return result;
}

std::wstring Engine::outputError() {
    std::scoped_lock lock(errorMutex_);
    return outputError_;
}

void Engine::runSource(std::size_t index, const std::wstring& output) {
    auto& source = *sources_[index];
    const auto kind = source.config.kind;
    const auto report = [&](HRESULT hr) {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) return;
        const auto context = kind == SourceKind::File ?
            tr(L"文件读取失败，请检查文件格式或访问权限", L"Could not read the file. Check its format and permissions.") :
            hr == E_ACCESSDENIED ?
            tr(L"无法安全地连接音源，请检查权限或设备路由", L"Could not safely connect the source. Check permissions or device routing.") :
            kind == SourceKind::Loopback ?
            tr(L"应用声音采集失败，请检查播放设备", L"Loopback capture failed. Check the playback device.") :
            tr(L"麦克风采集失败，请检查设备", L"Microphone capture failed. Check the device.");
        failSource(index, context, hr);
    };
    try {
        switch (kind) {
        case SourceKind::Microphone:
        case SourceKind::Loopback: capture(source, output); break;
        case SourceKind::File: decode(source); break;
        default: throw E_INVALIDARG;
        }
    } catch (HRESULT hr) {
        report(hr);
    } catch (const std::bad_alloc&) {
        report(E_OUTOFMEMORY);
    } catch (const std::exception&) {
        report(E_FAIL);
    }
}

void Engine::capture(Source& source, const std::wstring& output) {
    Com com;
    Event event;
    const bool loopback = source.config.kind == SourceKind::Loopback;
    const bool direct = output == virtualMicId;
    auto device = deviceFor(source.config.target);
    if (direct && !loopback) {
        ComPtr<IPropertyStore> properties;
        check(device->OpenPropertyStore(STGM_READ, &properties));
        if (isVirtualMic(properties.Get())) throw E_ACCESSDENIED;
    }
    // Recheck immediately before opening a loopback stream, including devices
    // that were unavailable during start(). Fail closed if output cannot resolve.
    if (!direct && loopback && sameEndpoint(endpointId(device.Get()), endpointId(deviceFor(output).Get())))
        throw E_ACCESSDENIED;
    auto audio = client(device.Get(), loopback ? eRender : eCapture);
    auto format = monoFormat();
    WAVEFORMATEX* native{};
    if (loopback) check(audio->GetMixFormat(&native));
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> storage(native, CoTaskMemFree);
    const auto samples = native ? mixFormat(*native) : mixFormat(format);
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : conversion);
    check(audio->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, loopback ? 0 : 200000, 0,
                            native ? native : &format, nullptr));
    check(audio->SetEventHandle(event.handle));
    ComPtr<IAudioCaptureClient> input;
    check(audio->GetService(IID_PPV_ARGS(&input)));
    AudioPriority priority;
    Started started(audio.Get());
    bool firstPacket = true;
    Resampler resampler;
    const HANDLE events[]{stopEvent_, event.handle};
    for (;;) {
        const auto wait = WaitForMultipleObjects(2, events, FALSE, loopback ? 100 : 3000);
        if (wait == WAIT_OBJECT_0) return;
        // No playback means no events; polling still detects invalidated endpoints.
        if (wait == WAIT_TIMEOUT) {
            if (!loopback) throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            DWORD state{};
            check(device->GetState(&state));
            if (state != DEVICE_STATE_ACTIVE) throw AUDCLNT_E_DEVICE_INVALIDATED;
        } else if (wait != WAIT_OBJECT_0 + 1) {
            throw HRESULT_FROM_WIN32(GetLastError());
        }
        for (;;) {
            UINT32 pending{};
            check(input->GetNextPacketSize(&pending));
            if (!pending) break;
            BYTE* data{};
            UINT32 frames{};
            DWORD packetFlags{};
            const auto hr = input->GetBuffer(&data, &frames, &packetFlags, nullptr, nullptr);
            check(hr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            const bool silent = (packetFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (!silent && frames && !data) {
                check(input->ReleaseBuffer(frames));
                throw E_UNEXPECTED;
            }
            bool overflow{};
            if (!firstPacket && (packetFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) {
                ++source.epoch;
                resampler = {};
            }
            firstPacket = false;
            const auto emit = [&](float value) { overflow = !source.queue.push(value) || overflow; };
            for (UINT32 i = 0; i < frames && !overflow; ++i) {
                const auto value = silent ? 0.0f : samples.mono(data + std::size_t(i) * samples.alignment);
                if (samples.sampleRate == rate) emit(value);
                else resampler.next(value, samples.sampleRate, emit);
            }
            if (overflow) {
                ++source.epoch;
                resampler = {};
            }
            check(input->ReleaseBuffer(frames));
            if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) return;
        }
    }
}

struct Engine::MixState {
    struct Track {
        ClockFollower clock;
        VoiceEq eq;
        Ducker ducker;
        unsigned epoch{};
    };
    std::array<Track, maxSources> tracks;
    PeakLimiter limiter;
};

void Engine::mixBlock(MixState& state, float* dest, unsigned frames, bool muted) {
    struct BlockTrack {
        Source* source{};
        float gain{}, peak{};
        bool file{}, mic{}, mute{}, eq{}, duck{}, enabled{};
    };
    std::array<BlockTrack, maxSources> block;
    for (std::size_t j = 0; j < sourceCount_; ++j) {
        auto& track = block[j];
        track.source = sources_[j].get();
        auto& source = *track.source;
        auto& dsp = state.tracks[j];
        auto& ctl = source.controls;
        track.file = source.config.kind == SourceKind::File;
        track.mic = source.config.kind == SourceKind::Microphone;
        track.enabled = !ctl.failed.load() && (!track.file || ctl.playing.load());
        const auto gain = ctl.gain.load();
        track.gain = std::isfinite(gain) ? std::clamp(gain, 0.0f, 2.0f) : 0.0f;
        track.mute = ctl.mute.load();
        track.eq = track.mic && ctl.voiceEq.load();
        track.duck = track.file && ctl.ducking.load();
        const auto epoch = source.epoch.load();
        if (!track.file && epoch != dsp.epoch) {
            source.queue.consume(source.queue.size());
            dsp.clock = {};
            dsp.eq.resetSignal();
            dsp.epoch = epoch;
        }
    }

    float outputPeak{};
    for (unsigned i = 0; i < frames; ++i) {
        float micLevel{}, sum{};
        bool micReady{};
        for (std::size_t j = 0; j < sourceCount_; ++j) {
            auto& track = block[j];
            if (track.file || !track.enabled) continue;
            auto& dsp = state.tracks[j];
            auto sample = dsp.clock.next(track.source->queue);
            if (track.mic) {
                if (!dsp.clock.ready()) dsp.eq.resetSignal();
                sample = dsp.eq.next(sample, track.eq);
            }
            if (track.mute) continue;
            sample *= track.gain;
            const auto level = std::abs(sample);
            track.peak = std::max(track.peak, level);
            sum += sample;
            if (track.mic && dsp.clock.ready()) {
                micLevel = std::max(micLevel, level);
                micReady = true;
            }
        }
        for (std::size_t j = 0; j < sourceCount_; ++j) {
            auto& track = block[j];
            if (!track.file) continue;
            const auto duck = state.tracks[j].ducker.next(micLevel, track.duck && micReady);
            if (!track.enabled) continue;
            float sample{};
            track.source->queue.pop(sample);
            if (track.mute) continue;
            sample *= track.gain * duck;
            track.peak = std::max(track.peak, std::abs(sample));
            sum += sample;
        }
        const auto limited = state.limiter.next(sum);
        dest[i] = muted ? 0.0f : limited;
        outputPeak = std::max(outputPeak, std::abs(dest[i]));
    }
    for (std::size_t j = 0; j < sourceCount_; ++j) {
        auto& track = block[j];
        auto& ctl = track.source->controls;
        ctl.peak = track.peak;
        if (ctl.failed.load()) ctl.peak = 0.0f;
    }
    outPeak = outputPeak;
}

void Engine::renderVirtual() {
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
        if (muted) {
            if (!wasMuted) writer.flush();
            outPeak = 0;
        }
        wasMuted = muted;
        const auto state = writer.query();
        ready_ = true;
        const auto frames = !state.active ? MPMIC_BLOCK_FRAMES / 2 :
            std::min(MPMIC_BLOCK_FRAMES, target - std::min(target, state.queuedFrames));
        if (!frames) continue;
        mixBlock(mix, audio.data(), frames, muted);
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

void Engine::render(std::wstring id) {
    const bool direct = id == virtualMicId;
    const auto report = [&](HRESULT hr) {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) return;
        failOutput(direct ? tr(L"与麦克风（声搭）的连接已停止，请检查驱动、权限或其他写入程序", L"MicPatch Microphone disconnected. Check the driver, permissions, or other writers.")
                          : tr(L"输出已停止，请检查选中的播放设备", L"Output stopped. Check the selected playback device."), hr);
        SetEvent(stopEvent_);
    };
    try {
        if (direct) renderVirtual();
        else renderWasapi(id);
    } catch (HRESULT hr) {
        report(hr);
    } catch (const std::bad_alloc&) {
        report(E_OUTOFMEMORY);
    } catch (const std::exception&) {
        report(E_FAIL);
    }
    for (std::size_t i = 0; i < sourceCount_; ++i) sources_[i]->controls.peak = 0;
    outPeak = 0;
    ready_ = false;
    active_ = false;
}

void Engine::renderWasapi(const std::wstring& id) {
    Com com;
    Event event;
    auto audio = client(deviceFor(id).Get(), eRender);
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
        if (padding > capacity) throw E_UNEXPECTED;
        const auto frames = capacity - padding;
        if (!frames) continue;
        check(output->GetBuffer(frames, &data));
        auto dest = reinterpret_cast<float*>(data);
        mixBlock(mix, dest, frames, outMute.load());
        const bool silent = outMute.load() || outPeak.load() == 0;
        if (silent) outPeak = 0;
        check(output->ReleaseBuffer(frames, silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0));
    }
}

void Engine::decode(Source& source) {
    Com com;
    Mf mf;
    ComPtr<IMFSourceReader> reader;
    const auto stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    check(MFCreateSourceReaderFromURL(source.config.target.c_str(), nullptr, &reader));
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
    SampleFormat format;
    const auto readFormat = [&] {
        ComPtr<IMFMediaType> actual;
        check(reader->GetCurrentMediaType(stream, &actual));
        GUID subtype{};
        check(actual->GetGUID(MF_MT_SUBTYPE, &subtype));
        if (subtype != MFAudioFormat_Float && subtype != MFAudioFormat_PCM) throw MF_E_INVALIDMEDIATYPE;
        format.floating = subtype == MFAudioFormat_Float;
        check(actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &format.channels));
        check(actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &format.sampleRate));
        check(actual->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &format.alignment));
        check(actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &format.bits));
        format.validate();
    };
    readFormat();
    constexpr auto stopped = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const auto enqueue = [&](float value) {
        while (!source.queue.push(value)) {
            if (WaitForSingleObject(stopEvent_, 4) == WAIT_OBJECT_0) throw stopped;
        }
    };
    Resampler resampler;
    const auto convert = [&](float value) {
        if (format.sampleRate == rate) enqueue(value);
        else resampler.next(value, format.sampleRate, enqueue);
    };
    bool eof{}, restart{}, hadSamples{};
    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        if (!source.controls.playing || (eof && !source.controls.loop && source.queue.size() != 0)) {
            WaitForSingleObject(stopEvent_, 4);
            continue;
        }
        if (eof) {
            if (!hadSamples) throw MF_E_INVALIDMEDIATYPE;
            if (!source.controls.loop) {
                source.controls.playing = false;
                restart = true;
                eof = false;
                continue;
            }
            restart = true;
        }
        if (restart) {
            seekStart(reader.Get());
            restart = eof = hadSamples = false;
            resampler = {};
        }
        DWORD flags{};
        ComPtr<IMFSample> sample;
        check(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample));
        if (flags & MF_SOURCE_READERF_ERROR) throw E_FAIL;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            readFormat();
            resampler = {};
        }
        if (sample) {
            ComPtr<IMFMediaBuffer> buffer;
            check(sample->ConvertToContiguousBuffer(&buffer));
            BYTE* data{};
            DWORD bytes{};
            check(buffer->Lock(&data, nullptr, &bytes));
            try {
                if (bytes % format.alignment || (bytes && !data)) throw MF_E_INVALIDMEDIATYPE;
                for (DWORD offset = 0; offset < bytes; offset += format.alignment)
                    convert(format.mono(data + offset));
                hadSamples = hadSamples || bytes != 0;
            } catch (...) {
                buffer->Unlock();
                throw;
            }
            check(buffer->Unlock());
        }
        eof = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
        if (eof && resampler.havePrevious) convert(resampler.previous);
    }
}
}
