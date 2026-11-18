#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace micpatch {

enum class Theme { Graphite, Ocean, Light };
enum class MuteHotkey { Standard, Alternate, Off };

struct Settings {
    bool english{};
    Theme theme{Theme::Graphite};
    MuteHotkey hotkey{MuteHotkey::Standard};
    bool rememberOutput{};
    bool startMuted{};
    std::wstring outputId;
};

// One versioned value keeps preferences and the remembered endpoint in sync.
inline std::wstring serializeSettings(const Settings& settings) {
    std::wstring value = L"1\n";
    const auto field = [&](int n) { value.push_back(static_cast<wchar_t>(L'0' + n)); value += L'\n'; };
    field(settings.english);
    field(static_cast<int>(settings.theme));
    field(static_cast<int>(settings.hotkey));
    field(settings.rememberOutput);
    field(settings.startMuted);
    if (settings.rememberOutput) value += settings.outputId;
    return value;
}

inline Settings parseSettings(std::wstring_view value) {
    const auto field = [&]() {
        if (value.size() < 2 || value[1] != L'\n' || value[0] < L'0' || value[0] > L'9') return -1;
        const int n = value[0] - L'0';
        value.remove_prefix(2);
        return n;
    };
    if (field() != 1) return {};
    const int language = field(), theme = field(), hotkey = field(), remember = field(), muted = field();
    if (language < 0 || language > 1 || theme < 0 || theme > 2 || hotkey < 0 || hotkey > 2 ||
        remember < 0 || remember > 1 || muted < 0 || muted > 1 || value.find(L'\0') != value.npos) return {};
    Settings settings{language != 0, static_cast<Theme>(theme), static_cast<MuteHotkey>(hotkey),
                      remember != 0, muted != 0, {}};
    if (settings.rememberOutput) settings.outputId = value;
    return settings;
}

inline constexpr wchar_t settingsKey[] = L"Software\\MicPatch";
inline constexpr wchar_t settingsValue[] = L"Preferences";
inline constexpr DWORD maxSettingsBytes = 128 * 1024;

inline Settings loadSettings(const wchar_t* key = settingsKey) {
    DWORD bytes{};
    if (RegGetValueW(HKEY_CURRENT_USER, key, settingsValue, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t) || bytes >= maxSettingsBytes || bytes % sizeof(wchar_t) != 0) return {};
    // Extra space permits RegGetValueW to terminate a malformed REG_SZ safely.
    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, key, settingsValue, RRF_RT_REG_SZ, nullptr, data.data(), &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0 || data[bytes / sizeof(wchar_t) - 1] != L'\0') return {};
    return parseSettings({data.data(), bytes / sizeof(wchar_t) - 1});
}

inline LSTATUS saveSettings(const Settings& settings, const wchar_t* key = settingsKey) {
    const auto value = serializeSettings(settings);
    const auto bytes = (value.size() + 1) * sizeof(wchar_t);
    if (bytes >= maxSettingsBytes) return ERROR_INVALID_DATA;
    HKEY handle{};
    const auto open = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                      KEY_SET_VALUE, nullptr, &handle, nullptr);
    if (open != ERROR_SUCCESS) return open;
    const auto result = RegSetValueExW(handle, settingsValue, 0, REG_SZ,
                                       reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>(bytes));
    RegCloseKey(handle);
    return result;
}

}  // namespace micpatch
