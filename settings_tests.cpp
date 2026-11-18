#include "settings.h"

#include <cstdio>

using namespace micpatch;

int main() {
    int failures{};
    const auto expect = [&](bool ok, const char* label) {
        if (!ok) { std::fprintf(stderr, "%s\n", label); ++failures; }
    };
    const auto defaults = parseSettings(L"");
    expect(!defaults.english && defaults.theme == Theme::Graphite && defaults.hotkey == MuteHotkey::Standard &&
        !defaults.rememberOutput && defaults.outputId.empty() && !defaults.startMuted, "safe Chinese defaults");
    const Settings configured{true, Theme::Ocean, MuteHotkey::Alternate, true, true, L"endpoint-设备"};
    const auto value = serializeSettings(configured);
    const auto parsed = parseSettings(value);
    expect(parsed.english && parsed.theme == Theme::Ocean && parsed.hotkey == MuteHotkey::Alternate &&
        parsed.rememberOutput && parsed.startMuted && parsed.outputId == configured.outputId, "settings round trip");
    auto invalid = value;
    invalid[0] = L'9';
    expect(!parseSettings(invalid).rememberOutput, "unknown schema rejects remembered output");
    invalid = value;
    invalid[4] = L'9';
    expect(parseSettings(invalid).theme == Theme::Graphite, "invalid theme falls back");
    auto notRemembered = configured;
    notRemembered.rememberOutput = false;
    expect(parseSettings(serializeSettings(notRemembered)).outputId.empty(), "disabled storage clears the endpoint");
    const std::wstring key = L"Software\\MicPatch-Test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    expect(loadSettings(key.c_str()).outputId.empty(), "missing registry value falls back");
    expect(saveSettings(configured, key.c_str()) == ERROR_SUCCESS, "isolated registry save");
    expect(serializeSettings(loadSettings(key.c_str())) == value, "isolated registry reload");
    expect(RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()) == ERROR_SUCCESS, "remove isolated test key");
    return failures != 0;
}
