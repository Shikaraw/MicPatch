#pragma once

#include "mix.h"

#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <windows.h>

namespace micpatch {
inline constexpr wchar_t virtualMicId[] = L"micpatch:virtual-mic";
struct Endpoint { std::wstring id, name; };
std::vector<Endpoint> endpoints(bool capture);
HRESULT virtualCaptureName(const std::wstring& instance, std::wstring& name);

class Engine {
public:
    ~Engine();
    std::wstring start(const std::wstring& output, const std::wstring& mic, const std::wstring& file);
    void stop();
    bool running() const { return ready_.load(); }
    std::wstring error();

    std::atomic<float> micGain{1}, fileGain{1}, micPeak{}, filePeak{}, outPeak{};
    std::atomic<bool> micMute{}, fileMute{}, outMute{}, playing{}, loop{}, voiceEq{}, ducking{};

private:
    struct MixState;
    void mixBlock(MixState& state, float* dest, unsigned frames, bool hasMic, bool hasFile, bool muted);
    void renderVirtual(bool hasMic, bool hasFile);
    void renderWasapi(const std::wstring& id, bool hasMic, bool hasFile);
    void render(std::wstring id, bool hasMic, bool hasFile);
    void capture(std::wstring id);
    void decode(std::wstring path);
    void fail(const wchar_t* context, HRESULT code);

    Ring<8192> micQueue_;
    Ring<65536> fileQueue_;
    std::atomic<bool> active_{}, ready_{}, micLost_{}, fileLost_{};
    std::atomic<unsigned> micEpoch_{};
    std::mutex errorMutex_;
    std::wstring error_;
    std::thread renderThread_, captureThread_, fileThread_;
    HANDLE stopEvent_{};
};
}
