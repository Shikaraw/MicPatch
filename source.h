#pragma once

#include <cstddef>
#include <string>

namespace micpatch {
enum class SourceKind { Microphone, File, Loopback };
inline constexpr std::size_t maxSources = 16;

struct SourceConfig {
    SourceKind kind{};
    std::wstring target;
    int gain{100};
    bool mute{}, loop{}, voiceEq{}, ducking{};
};
}
