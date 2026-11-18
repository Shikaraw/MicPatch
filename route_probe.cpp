#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

struct Failure { std::wstring message; };

void check(HRESULT hr, const wchar_t* operation) {
    if (SUCCEEDED(hr)) return;
    wchar_t code[16]{};
    std::swprintf(code, 16, L"0x%08lX", static_cast<unsigned long>(hr));
    throw Failure{std::wstring(operation) + L" failed: " + code};
}

struct Apartment {
    // endpoints() initializes COM as MTA internally, so use the same model here.
    Apartment() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), L"CoInitializeEx"); }
    ~Apartment() { CoUninitialize(); }
};

void list(bool capture) {
    std::wcout << (capture ? L"Capture endpoints:\n" : L"Render endpoints:\n");
    for (const auto& ep : micpatch::endpoints(capture))
        std::wcout << L"  " << ep.name << L"\n    " << ep.id << L'\n';
}

micpatch::Endpoint resolve(const std::wstring& selector, bool capture) {
    const std::wstring kind = capture ? L"capture" : L"render";
    if (selector.empty()) throw Failure{L"Empty " + kind + L" selector"};
    const auto devices = micpatch::endpoints(capture);
    for (const auto& ep : devices) {
        if (!ep.id.empty() && CompareStringOrdinal(ep.id.c_str(), -1, selector.c_str(), -1, TRUE)
                                  == CSTR_EQUAL)
            return ep;
    }
    const micpatch::Endpoint* match = nullptr;
    for (const auto& ep : devices) {
        if (ep.id.empty() || FindStringOrdinal(FIND_FROMSTART, ep.name.c_str(),
            static_cast<int>(ep.name.size()), selector.c_str(), static_cast<int>(selector.size()),
            TRUE) < 0) continue;
        if (match) throw Failure{L"Ambiguous " + kind + L" name: " + selector + L" (use an ID)"};
        match = &ep;
    }
    if (!match) throw Failure{L"No matching " + kind + L" endpoint: " + selector};
    return *match;
}

ComPtr<IMMDevice> openEndpoint(IMMDeviceEnumerator& enumerator,
                               const micpatch::Endpoint& ep, EDataFlow expected) {
    ComPtr<IMMDevice> device;
    check(enumerator.GetDevice(ep.id.c_str(), device.GetAddressOf()), L"IMMDeviceEnumerator::GetDevice");
    ComPtr<IMMEndpoint> endpoint;
    check(device.As(&endpoint), L"QueryInterface(IMMEndpoint)");
    EDataFlow flow{};
    check(endpoint->GetDataFlow(&flow), L"IMMEndpoint::GetDataFlow");
    DWORD state{};
    check(device->GetState(&state), L"IMMDevice::GetState");
    if (flow != expected || state != DEVICE_STATE_ACTIVE)
        throw Failure{L"Endpoint is inactive or has the wrong direction: " + ep.id};
    return device;
}

struct Capture {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> reader;
    unsigned rate{}, channels{}, bytes{}, stride{};
    bool floating{}, started{};

    explicit Capture(IMMDevice& device) {
        check(device.Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client.GetAddressOf())), L"Activate(IAudioClient)");
        WAVEFORMATEX* raw = nullptr;
        check(client->GetMixFormat(&raw), L"IAudioClient::GetMixFormat");
        std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(raw, CoTaskMemFree);
        const auto& f = *format;
        unsigned tag = f.wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE && f.cbSize >= 22) {
            const auto& x = *reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(raw);
            constexpr GUID pcm{DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_PCM)};
            constexpr GUID ieee{DEFINE_WAVEFORMATEX_GUID(WAVE_FORMAT_IEEE_FLOAT)};
            if (IsEqualGUID(x.SubFormat, pcm)) tag = WAVE_FORMAT_PCM;
            if (IsEqualGUID(x.SubFormat, ieee)) tag = WAVE_FORMAT_IEEE_FLOAT;
        }
        floating = tag == WAVE_FORMAT_IEEE_FLOAT;
        const auto bits = f.wBitsPerSample;
        rate = f.nSamplesPerSec;
        channels = f.nChannels;
        bytes = bits / 8;
        stride = f.nBlockAlign;
        if (!(floating ? (bits == 32 || bits == 64)
                       : (tag == WAVE_FORMAT_PCM && (bits == 8 || bits == 16 || bits == 24 || bits == 32)))
            || rate < 1000 || !channels || stride != channels * bytes)
            throw Failure{L"Unsupported capture mix format (expected PCM or IEEE float)"};
        // Shared-mode capture on the named capture endpoint, not render-loopback capture.
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2'000'000, 0, raw, nullptr),
              L"IAudioClient::Initialize(capture)");
        check(client->GetService(IID_PPV_ARGS(reader.GetAddressOf())), L"GetService(IAudioCaptureClient)");
    }

    ~Capture() { if (started) client->Stop(); }

    void start() {
        check(client->Start(), L"IAudioClient::Start(capture)");
        started = true;
    }

    void stopAndReset() {
        check(client->Stop(), L"IAudioClient::Stop(capture)");
        started = false;
        check(client->Reset(), L"IAudioClient::Reset(capture)");
    }

    double sample(const BYTE* data) const {
        if (floating) {
            double value;
            if (bytes == 4) {
                float x;
                std::memcpy(&x, data, sizeof x);
                value = x;
            } else {
                std::memcpy(&value, data, sizeof value);
            }
            if (!std::isfinite(value)) throw Failure{L"Non-finite sample from capture endpoint"};
            return value;
        }
        if (bytes == 1) return (static_cast<int>(*data) - 128) / 128.0;
        // PCM is little-endian; extensible PCM puts valid bits at the top of the container.
        std::int64_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) value |= std::int64_t(data[i]) << (8 * i);
        const auto midpoint = std::int64_t{1} << (8 * bytes - 1);
        if (value >= midpoint) value -= 2 * midpoint;
        return static_cast<double>(value) / static_cast<double>(midpoint);
    }

    template<class Packet, class Health>
    bool until(Clock::time_point deadline, Packet packet, Health health) {
        while (Clock::now() < deadline) {
            health();
            UINT32 available{};
            check(reader->GetNextPacketSize(&available), L"IAudioCaptureClient::GetNextPacketSize");
            if (!available) {
                std::this_thread::sleep_for(5ms);
                continue;
            }
            BYTE* data{};
            UINT32 frames{};
            DWORD flags{};
            check(reader->GetBuffer(&data, &frames, &flags, nullptr, nullptr), L"IAudioCaptureClient::GetBuffer");
            bool done;
            try {
                done = packet(data, frames, flags);
            } catch (...) {
                reader->ReleaseBuffer(frames);
                throw;
            }
            check(reader->ReleaseBuffer(frames), L"IAudioCaptureClient::ReleaseBuffer");
            if (done && Clock::now() < deadline) return true;
        }
        return false;
    }
};

struct Reading {
    double rms{}, toneFraction{}; // AC RMS of strongest channel, and its energy fraction near 440 Hz.
};

struct Meter {
    struct Sum { double x{}, xx{}, c{}, s{}; };
    const Capture& capture;
    const unsigned window;
    const bool tone;
    std::vector<Sum> sums;
    std::vector<double> sine, cosine;
    double sineMean{}, cosineMean{};
    unsigned n{};

    Meter(const Capture& c, bool useTone)
        : capture(c), window((c.rate + 2) / 4), tone(useTone), sums(c.channels) {
        if (!tone) return;
        for (unsigned i = 0; i < window; ++i) {
            const double phase = 2.0 * std::numbers::pi * 440.0 * i / c.rate;
            sineMean += sine.emplace_back(std::sin(phase));
            cosineMean += cosine.emplace_back(std::cos(phase));
        }
        sineMean /= window;
        cosineMean /= window;
    }

    void reset() { n = 0; std::fill(sums.begin(), sums.end(), Sum{}); }

    template<class Window, class Gap>
    bool packet(const BYTE* data, UINT32 frames, DWORD flags, Window consume, Gap gap) {
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) { reset(); gap(); }
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        if (!silent && frames && !data) throw Failure{L"Missing capture buffer"};
        for (UINT32 i = 0; i < frames; ++i) {
            if (!silent) {
                const auto* frame = data + static_cast<size_t>(i) * capture.stride;
                for (unsigned ch = 0; ch < capture.channels; ++ch) {
                    const double x = capture.sample(frame + ch * capture.bytes);
                    auto& sum = sums[ch];
                    sum.x += x;
                    sum.xx += x * x;
                    if (tone) { sum.c += x * cosine[n]; sum.s += x * sine[n]; }
                }
            }
            if (++n != window) continue;
            Reading read;
            for (const auto& sum : sums) {
                const double power = std::max(0.0, sum.xx / n - (sum.x / n) * (sum.x / n));
                const double rms = std::sqrt(power);
                if (rms <= read.rms) continue;
                read = {rms, 0};
                if (tone) {
                    const double c = (sum.c - sum.x * cosineMean) / n;
                    const double s = (sum.s - sum.x * sineMean) / n;
                    // A quarter-second window contains approximately 110 full 440 Hz cycles.
                    read.toneFraction = std::min(1.0, 2.0 * (c * c + s * s) / power);
                }
            }
            reset();
            if (consume(read)) return true;
        }
        return false;
    }
};

struct EngineSession {
    micpatch::Engine engine;
    bool startCalled{};
    ~EngineSession() { if (startCalled) engine.stop(); }
};

void printReading(const wchar_t* label, Reading r, bool tone) {
    std::wcout << label << L": RMS=" << std::fixed << std::setprecision(6) << r.rms;
    if (tone) std::wcout << L", 440 Hz energy=" << std::setprecision(1) << 100 * r.toneFraction << L'%';
    std::wcout << std::endl;
}

int run(int argc, wchar_t** argv) {
    const bool tone = argc == 5 && std::wstring(argv[4]) == L"--tone440";
    if (argc != 1 && argc != 4 && !tone) {
        std::wcerr << L"Usage: route_probe [<render ID/name> <capture ID/name> <audio file> [--tone440]]\n";
        return 2;
    }
    Apartment com;
    if (argc == 1) { list(false); list(true); return 0; }
    const std::wstring file = argv[3];
    const DWORD attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        throw Failure{L"Test audio file does not exist or is not a file: " + file};

    const auto render = resolve(argv[1], false);
    const auto input = resolve(argv[2], true);
    std::wcout << L"Render: " << render.name << L" [" << render.id << L"]\nCapture: "
               << input.name << L" [" << input.id << L"]" << std::endl;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           IID_PPV_ARGS(enumerator.GetAddressOf())), L"CoCreateInstance(MMDeviceEnumerator)");
    if (render.id != micpatch::virtualMicId) openEndpoint(*enumerator.Get(), render, eRender);
    const auto captureDevice = openEndpoint(*enumerator.Get(), input, eCapture);
    Capture capture(*captureDevice.Get());
    Meter meter(capture, tone);
    auto measure = [&](Clock::duration budget, auto consume, auto gap, auto health) {
        meter.reset();
        return capture.until(Clock::now() + budget, [&](const BYTE* data, UINT32 frames, DWORD flags) {
            return meter.packet(data, frames, flags, consume, gap);
        }, health);
    };

    capture.start();
    Reading baseline;
    unsigned count = 0;
    if (!measure(3s, [&](Reading r) {
            if (r.rms >= baseline.rms) baseline = r;
            return ++count == 3;
        }, [&] { count = 0; baseline = {}; }, [] {}))
        throw Failure{L"Timed out collecting continuous baseline samples"};
    printReading(L"Baseline (max)", baseline, tone);
    capture.stopAndReset();

    EngineSession session;
    auto& engine = session.engine;
    engine.fileGain = 1.0f;
    engine.fileMute = false;
    engine.outMute = false;
    engine.loop = true;
    engine.playing = true;
    const auto startupDeadline = Clock::now() + 6s;
    session.startCalled = true;
    if (auto error = engine.start(render.id, {}, file); !error.empty())
        throw Failure{L"Engine start: " + error};
    auto engineError = [&] {
        if (auto error = engine.error(); !error.empty()) throw Failure{L"Engine: " + error};
    };
    for (;;) {
        engineError();
        if (Clock::now() >= startupDeadline) throw Failure{L"Timed out waiting for the engine to start"};
        if (engine.running()) break;
        std::this_thread::sleep_for(10ms);
    }
    engine.loop = true;
    engine.playing = true;
    const auto health = [&] {
        engineError();
        if (!engine.running()) throw Failure{L"Engine stopped during measurement"};
    };
    capture.start();

    const double minSignal = tone ? std::max({0.001, 2.0 * baseline.rms,
                                               4.0 * baseline.rms * std::sqrt(baseline.toneFraction)})
                                  : std::max(0.001, 4.0 * baseline.rms);
    auto stable = [&](const wchar_t* label, double minimum) {
        unsigned streak = 0;
        Reading low, high, last;
        const auto reset = [&] { streak = 0; };
        const bool ok = measure(5s, [&](Reading r) {
            last = r;
            if (r.rms < minimum || (tone && r.toneFraction < 0.65)) { reset(); return false; }
            if (streak && std::max(high.rms, r.rms) <= 1.414214 * std::min(low.rms, r.rms)) {
                if (r.rms < low.rms) low = r;
                if (r.rms > high.rms) high = r;
                ++streak;
            } else {
                low = high = r;
                streak = 1;
            }
            return streak == 3;
        }, reset, health);
        if (!ok) {
            printReading(L"Last capture window", last, tone);
            throw Failure{std::wstring(L"Timed out waiting for stable qualifying signal: ") + label};
        }
        printReading(label, low, tone);
        return low.rms;
    };
    const double active = stable(L"Playing (stable min)", minSignal);
    engine.outMute = true;
    // Consume (do not measure) in-flight audio for the first 500 ms after the toggle.
    capture.until(Clock::now() + 500ms, [](const BYTE*, UINT32, DWORD) { return false; }, health);
    const double muteLimit = 0.1 * active;
    Reading muted, lastMuted;
    count = 0;
    auto clearMute = [&] { muted = {}; count = 0; };
    if (!measure(4s, [&](Reading r) {
            lastMuted = r;
            if (r.rms > muteLimit) { clearMute(); return false; }
            if (r.rms >= muted.rms) muted = r;
            return ++count == 4;
        }, clearMute, health)) {
        printReading(L"Last muted capture window", lastMuted, tone);
        throw Failure{L"Timed out waiting for at least 20 dB attenuation with outMute"};
    }
    printReading(L"Muted (max)", muted, tone);
    engine.outMute = false;
    stable(L"Unmuted (stable min)", std::max(minSignal, 0.707107 * active));
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    try {
        // Normal paths clean up via RAII. A stuck COM call or Engine::stop must not hang the CLI.
        std::jthread watchdog([](std::stop_token token) {
            std::mutex mutex;
            std::unique_lock lock(mutex);
            std::condition_variable_any wake;
            wake.wait_for(lock, token, 30s, [] { return false; });
            if (!token.stop_requested()) TerminateProcess(GetCurrentProcess(), 124);
        });
        const int status = run(argc, argv);
        if (status == 0 && argc > 1) std::wcout << L"PASS: captured signal followed outMute on/off" << std::endl;
        return status;
    } catch (const Failure& e) {
        std::wcerr << L"FAIL: " << e.message << std::endl;
    } catch (const std::exception& e) {
        std::wcerr << L"FAIL: " << e.what() << std::endl;
    } catch (...) {
        std::wcerr << L"FAIL: unexpected exception\n";
    }
    return 1;
}
