#include "mix.h"
#include "virtual_mic_protocol.h"

#include <cstdio>
#include <limits>

using namespace micpatch;

int main() {
    int failures{};
    auto expect = [&](bool condition, const char* message) {
        if (!condition) { std::fprintf(stderr, "%s\n", message); ++failures; }
    };
    expect(MPMIC_RATE == rate && MPMIC_BLOCK_FRAMES == rate / 100, "virtual microphone format");
    expect(virtualPcm16(0) == 0 && virtualPcm16(0.5f) == 16384 && virtualPcm16(-0.5f) == -16384,
           "PCM16 silence and rounding");
    expect(virtualPcm16(2) == 32767 && virtualPcm16(-2) == -32768, "PCM16 clamps before conversion");
    expect(virtualPcm16(std::numeric_limits<float>::quiet_NaN()) == 0, "PCM16 rejects NaN");
    expect(scaled(0.25f, 2, false) == 0.5f, "gain");
    expect(scaled(1, 2, true) == 0, "mute");
    expect(scaled(std::numeric_limits<float>::infinity(), 1, false) == 0, "non-finite input");
    PeakLimiter limiter;
    expect(limiter.next(scaled(0.15f, 2, false) + scaled(0.2f, 1, false)) == 0.5f, "mix sum");
    expect(limiter.next(4) == 0.98f, "positive peak ceiling");
    expect(limiter.next(-4) == -0.98f, "negative peak ceiling");
    for (unsigned i = 0; i < rate; ++i) limiter.next(0);
    expect(limiter.next(0.5f) > 0.499f, "limiter recovers");

    VoiceEq bypass;
    expect(bypass.next(0.4f, false) == 0.4f, "EQ bypass is transparent");
    const auto response = [](float frequency) {
        VoiceEq eq;
        float input{}, output{};
        for (unsigned i = 0; i < rate; ++i) {
            const auto x = std::sin(6.2831853f * frequency * float(i) / rate);
            const auto y = eq.next(x, true);
            if (i < rate / 2) continue;
            input += x * x;
            output += y * y;
        }
        return std::sqrt(output / input);
    };
    expect(response(30) < 0.4f, "EQ attenuates rumble");
    expect(response(1000) > 0.95f, "EQ preserves speech midrange");
    expect(response(5000) > 1.12f, "EQ gently boosts presence");
    for (unsigned i = 0; i < rate / 20; ++i) bypass.next(0.5f, true);
    bypass.resetSignal();
    expect(bypass.next(0, true) == 0, "EQ reset discards filter tail");
    for (unsigned i = 0; i < rate / 20; ++i) bypass.next(0.5f, true);
    const float beforeSwitch = bypass.next(0.5f, true);
    const float afterSwitch = bypass.next(0.5f, false);
    expect(std::abs(beforeSwitch - afterSwitch) < 0.01f, "EQ bypass transition uses a short ramp");
    float dry{};
    for (unsigned i = 0; i < rate / 20; ++i) dry = bypass.next(0.5f, false);
    expect(dry == 0.5f, "EQ returns to transparent bypass");

    Ducker ducker;
    expect(ducker.next(0.5f, false) == 1, "disabled ducking preserves accompaniment");
    expect(ducker.next(0.03f, true) == 1, "ducking ignores signal below entry threshold");
    float duckGain{};
    for (unsigned i = 0; i < rate / 20; ++i) duckGain = ducker.next(0.2f, true);
    expect(duckGain > 0.25f && duckGain < 0.26f, "ducking attenuates music by roughly 12 dB");
    for (unsigned i = 0; i < rate / 2; ++i) duckGain = ducker.next(0.03f, true);
    expect(duckGain < 0.26f, "ducking holds through threshold hysteresis");
    auto firstRelease = ducker.next(0, false);
    expect(firstRelease > duckGain && firstRelease < 0.27f, "muted or lost mic starts a smooth release");
    for (unsigned i = 0; i < rate / 2; ++i) duckGain = ducker.next(0, false);
    expect(duckGain > 0.97f, "ducking recovers when mic is muted or lost");
    for (unsigned i = 0; i < rate / 10; ++i) ducker.next(0.2f, true);
    expect(ducker.next(0, true) < 0.26f, "short speech gaps do not release ducking");
    for (unsigned i = 0; i < rate * 3 / 4; ++i) duckGain = ducker.next(0, true);
    expect(duckGain > 0.98f, "ducking releases after sustained silence");

    Ring<4> queue;
    float value{};
    expect(!queue.pop(value), "empty queue");
    for (int i = 0; i < 4; ++i) expect(queue.push(float(i)), "push until full");
    expect(!queue.push(9), "full queue does not overwrite");
    for (int i = 0; i < 4; ++i) {
        expect(queue.pop(value) && value == float(i), "FIFO order");
        expect(queue.push(float(i + 4)), "wrap around");
    }
    expect(queue.pop(value) && value == 4, "wrapped item");
    queue.reset();
    expect(!queue.pop(value), "reset discards stale audio");

    Ring<8192> capture;
    ClockFollower follower;
    expect(follower.next(capture) == 0, "capture starts in silence");
    for (unsigned i = 0; i < 1200; ++i) capture.push(1);
    expect(follower.next(capture) == 1, "capture primes at target");
    for (unsigned i = 0; i < 2000; ++i) follower.next(capture);
    expect(follower.next(capture) == 0, "underrun produces silence");
    for (unsigned i = 0; i < 1200; ++i) capture.push(1);
    bool stable = true;
    for (unsigned i = 0; i < rate * 30; ++i) {
        if (i % 500 != 0) stable &= capture.push(1);
        stable &= follower.next(capture) == 1;
    }
    expect(stable && capture.size() > 800 && capture.size() < 1600, "drift control tracks a slower producer");
    return failures != 0;
}
