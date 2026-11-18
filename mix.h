#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace micpatch {
inline constexpr unsigned rate = 48000;

// Exactly one producer and one consumer; reset only after both have stopped.
template<std::size_t Capacity>
class Ring {
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
    struct alignas(64) Cursor {
        std::atomic<std::size_t> value{};
        std::byte padding[64 - sizeof(value)]{};
    };
    alignas(64) std::array<float, std::max(Capacity, 64 / sizeof(float))> samples_{};
    Cursor write_, read_;

public:
    std::size_t size() const {
        const auto read = read_.value.load(std::memory_order_acquire);
        return write_.value.load(std::memory_order_acquire) - read;
    }
    bool push(float sample) {
        const auto write = write_.value.load(std::memory_order_relaxed);
        if (write - read_.value.load(std::memory_order_acquire) >= Capacity) return false;
        samples_[write & (Capacity - 1)] = sample;
        write_.value.store(write + 1, std::memory_order_release);
        return true;
    }
    float peek(std::size_t offset) const {
        return samples_[(read_.value.load(std::memory_order_relaxed) + offset) & (Capacity - 1)];
    }
    void consume(std::size_t count) {
        read_.value.store(read_.value.load(std::memory_order_relaxed) + count, std::memory_order_release);
    }
    bool pop(float& sample) {
        if (size() == 0) return false;
        sample = peek(0);
        consume(1);
        return true;
    }
    void reset() {
        read_.value.store(0, std::memory_order_relaxed);
        write_.value.store(0, std::memory_order_relaxed);
    }
};

class ClockFollower {
    static constexpr std::size_t target = rate / 40;
    double fraction_{};
    double step_{1};
    bool ready_{};

public:
    bool ready() const { return ready_; }

    template<std::size_t N>
    float next(Ring<N>& queue) {
        const auto available = queue.size();
        if (available < 3) {
            ready_ = false;
            fraction_ = 0;
        }
        if (!ready_) {
            if (available < target) return 0;
            ready_ = true;
            step_ = 1;
        }
        const auto desired = 1.0 + std::clamp((double(available) - target) / (target * 80.0), -0.02, 0.02);
        step_ += (desired - step_) * 0.001;
        const float a = queue.peek(0);
        const float b = queue.peek(1);
        const auto sample = a + (b - a) * static_cast<float>(fraction_);
        fraction_ += step_;
        const auto count = static_cast<std::size_t>(fraction_);
        queue.consume(count);
        fraction_ -= static_cast<double>(count);
        return sample;
    }
};

inline short virtualPcm16(float sample) {
    if (sample != sample) return 0;
    if (sample <= -1.0f) return -32768;
    if (sample >= 1.0f) return 32767;
    const float pcm = sample * (sample < 0.0f ? 32768.0f : 32767.0f);
    return static_cast<short>(pcm + (pcm < 0.0f ? -0.5f : 0.5f));
}

inline float scaled(float sample, float gain, bool mute) {
    return mute || !std::isfinite(sample) ? 0.0f : sample * std::clamp(gain, 0.0f, 2.0f);
}

class VoiceEq {
    float bass_{}, body_{}, blend_{};

public:
    void resetSignal() { bass_ = body_ = 0; }

    float next(float sample, bool enabled) {
        constexpr float blendStep = 1.0f / (rate * 0.015f);
        blend_ = enabled ? std::min(1.0f, blend_ + blendStep) : std::max(0.0f, blend_ - blendStep);
        // One-pole filters at approximately 90 Hz and 2.5 kHz.
        bass_ += 0.01171f * (sample - bass_);
        const float cut = sample - bass_;
        body_ += 0.27910f * (cut - body_);
        const float bright = cut + 0.26f * (cut - body_);
        return sample + blend_ * (bright - sample);
    }
};

class Ducker {
    float envelope_{}, gain_{1};
    unsigned hold_{};
    bool triggered_{};

public:
    float next(float mic, bool enabled) {
        const float peak = std::abs(mic);
        envelope_ = std::max(peak, envelope_ * 0.998959f);
        if (!enabled) {
            envelope_ = 0;
            hold_ = 0;
            triggered_ = false;
        } else if (envelope_ >= (triggered_ ? 0.025f : 0.04f)) {
            hold_ = rate / 10;
            triggered_ = true;
        } else if (hold_ > 0) {
            --hold_;
        } else {
            triggered_ = false;
        }
        const float target = triggered_ ? 0.2511886f : 1.0f;
        gain_ += (target - gain_) * (target < gain_ ? 1.0f / (rate * 0.010f) : 1.0f / (rate * 0.135f));
        return gain_;
    }
};

class PeakLimiter {
    float gain_{1};

public:
    float next(float sample) {
        constexpr float ceiling = 0.98f;
        const auto peak = std::abs(sample);
        const auto needed = peak > ceiling ? ceiling / peak : 1.0f;
        const auto released = gain_ + (1.0f - gain_) * (1.0f / (rate * 0.05f));
        gain_ = std::min(needed, released);
        return std::clamp(sample * gain_, -ceiling, ceiling);
    }
};
}
