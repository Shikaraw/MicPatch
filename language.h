#pragma once

#include <atomic>

namespace micpatch {
inline std::atomic<bool> english{};
inline const wchar_t* tr(const wchar_t* zh, const wchar_t* en) {
    return english.load(std::memory_order_relaxed) ? en : zh;
}
}
