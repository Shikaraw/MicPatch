#include "session.h"

#include <cstdio>

using namespace micpatch;

int main() {
    int failures{};
    const auto expect = [&](bool ok, const char* label) {
        if (!ok) { std::fprintf(stderr, "%s\n", label); ++failures; }
    };
    const auto defaults = parseSettings(L"");
    expect(!defaults.english && defaults.theme == Theme::Graphite && defaults.hotkey == MuteHotkey::Standard &&
        defaults.rememberOutput && defaults.outputId.empty() && !defaults.startMuted, "safe Chinese defaults");
    const Settings configured{true, Theme::Ocean, MuteHotkey::Alternate, true, true, L"endpoint-设备"};
    const auto value = serializeSettings(configured);
    const auto parsed = parseSettings(value);
    expect(parsed.english && parsed.theme == Theme::Ocean && parsed.hotkey == MuteHotkey::Alternate &&
        parsed.rememberOutput && parsed.startMuted && parsed.outputId == configured.outputId, "settings round trip");
    auto invalid = value;
    invalid[0] = L'9';
    expect(parseSettings(invalid).outputId.empty(), "unknown schema rejects remembered output");
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
    expect(!loadSettings(key.c_str()).outputId.empty() && !parseSettings(L"1\n0\n0\n0\n0\n0\n").rememberOutput,
           "legacy remember-output choice is preserved");
    const Session session{L"route", {{SourceKind::Microphone, L"mic-A", 82, true, false, true},
        {SourceKind::File, L"D:\\音频\\乐曲.wav", 90, false, true, false, true},
        {SourceKind::Loopback, L"speakers", 120}, {SourceKind::File, L"D:\\other.mp3"}}};
    const auto snapshot = serializeSession(session);
    auto restored = parseSession(snapshot);
    expect(restored && restored->sources.size() == 4 && serializeSession(*restored) == snapshot,
           "session round trip includes multiple ordered sources and options");
    expect(!parseSession(snapshot.substr(0, snapshot.size() - 1)), "truncated session rejected");
    expect(!parseSession(L"2\n0\n0\n") && !parseSession(L"1\n17\n0\n"), "unknown format and excess sources rejected");
    expect(!parseSession(L"1\n1\n0\n1\n201\n0\n0\n"), "invalid gain rejected");
    expect(autoSaveEnabled(key.c_str()) && saveSession(session, key.c_str()) == ERROR_SUCCESS,
           "automatic session persistence is on by default");
    expect(serializeSession(loadSession(key.c_str())) == snapshot, "session restored from separate registry value");
    expect(setAutoSaveEnabled(false, key.c_str()) == ERROR_SUCCESS && loadSession(key.c_str()).sources.empty(),
           "disabled session is not restored");
    expect(clearSession(key.c_str()) == ERROR_SUCCESS && setAutoSaveEnabled(true, key.c_str()) == ERROR_SUCCESS &&
           loadSession(key.c_str()).sources.empty(), "clear saved audio sources");
    expect(serializeSettings(loadSettings(key.c_str())) == value, "session does not modify preferences");
    expect(RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()) == ERROR_SUCCESS, "remove isolated test key");
    return failures != 0;
}
