#pragma once

#include "mix.h"
#include "source.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace micpatch {
inline constexpr wchar_t virtualMicId[] = L"micpatch:virtual-mic";
struct Endpoint { std::wstring id, name; };
std::vector<Endpoint> endpoints(bool capture);
HRESULT virtualCaptureName(const std::wstring& instance, std::wstring& name);

struct SourceControls {
    std::atomic<float> gain{1}, peak{};
    std::atomic<bool> mute{}, loop{}, voiceEq{}, ducking{}, playing{}, failed{};
};

class Engine {
public:
    Engine();
    ~Engine();
    std::wstring start(const std::wstring& output, const std::vector<SourceConfig>& sources);
    void stop();
    bool running() const { return ready_.load(); }
    // start, source and stop are called by the owner thread; use the atomics for live changes.
    SourceControls* source(std::size_t index);
    std::wstring error();
    std::wstring outputError();

    std::atomic<float> outPeak{};
    std::atomic<bool> outMute{};

private:
    struct Source;
    struct MixState;
    void mixBlock(MixState& state, float* dest, unsigned frames, bool muted);
    void renderVirtual();
    void renderWasapi(const std::wstring& id);
    void render(std::wstring id);
    void runSource(std::size_t index, const std::wstring& output);
    void capture(Source& source, const std::wstring& output);
    void decode(Source& source);
    void failSource(std::size_t index, const wchar_t* context, HRESULT code);
    void failOutput(const wchar_t* context, HRESULT code);

    std::array<std::unique_ptr<Source>, maxSources> sources_;
    std::size_t sourceCount_{};
    std::atomic<bool> active_{}, ready_{};
    std::mutex errorMutex_;
    std::array<std::wstring, maxSources> sourceErrors_;
    std::wstring outputError_;
    std::thread renderThread_;
    HANDLE stopEvent_{};
};
}
