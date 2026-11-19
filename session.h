#pragma once

#include "settings.h"
#include "source.h"
#include <optional>
#include <utility>

namespace micpatch {
struct Session {
    std::wstring outputId;
    std::vector<SourceConfig> sources;
};

inline constexpr wchar_t sessionValue[] = L"LastSession";
inline constexpr wchar_t autoSaveValue[] = L"AutoSaveSession";
inline constexpr DWORD maxSessionBytes = 2 * 1024 * 1024;
inline constexpr unsigned maxTargetLength = 32767;

inline std::wstring serializeSession(const Session& session) {
    std::wstring value;
    auto number = [&](std::size_t n) { value += std::to_wstring(n); value += L'\n'; };
    auto text = [&](const std::wstring& s) { number(s.size()); value += s; };
    number(1);
    number(session.sources.size());
    text(session.outputId);
    for (const auto& source : session.sources) {
        number(static_cast<unsigned>(source.kind));
        number(static_cast<unsigned>(source.gain));
        number((source.mute ? 1u : 0u) | (source.loop ? 2u : 0u) |
               (source.voiceEq ? 4u : 0u) | (source.ducking ? 8u : 0u));
        text(source.target);
    }
    return value;
}

inline std::optional<Session> parseSession(std::wstring_view value) {
    if (value.size() >= maxSessionBytes / sizeof(wchar_t) || value.find(L'\0') != value.npos) return {};
    auto number = [&](unsigned& n, unsigned max) {
        const auto end = value.find(L'\n');
        if (!end || end == value.npos || end > 5) return false;
        n = 0;
        for (const auto ch : value.substr(0, end)) {
            if (ch < L'0' || ch > L'9') return false;
            n = n * 10 + static_cast<unsigned>(ch - L'0');
        }
        value.remove_prefix(end + 1);
        return n <= max;
    };
    auto text = [&](std::wstring& s) {
        unsigned length{};
        if (!number(length, maxTargetLength) || length > value.size()) return false;
        s = value.substr(0, length);
        value.remove_prefix(length);
        return true;
    };
    unsigned version{}, count{};
    Session result;
    if (!number(version, 1) || version != 1 || !number(count, maxSources) || !text(result.outputId)) return {};
    result.sources.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        unsigned kind{}, gain{}, flags{};
        SourceConfig source;
        if (!number(kind, 2) || !number(gain, 200) || !number(flags, 15) || !text(source.target)) return {};
        source.kind = static_cast<SourceKind>(kind);
        source.gain = static_cast<int>(gain);
        source.mute = (flags & 1) != 0;
        source.loop = (flags & 2) != 0;
        source.voiceEq = (flags & 4) != 0;
        source.ducking = (flags & 8) != 0;
        result.sources.push_back(std::move(source));
    }
    if (!value.empty()) return {};
    return result;
}

inline LSTATUS writeSessionValue(const wchar_t* key, const wchar_t* name, DWORD type, const void* data, DWORD bytes) {
    HKEY handle{};
    auto status = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                  KEY_SET_VALUE, nullptr, &handle, nullptr);
    if (status != ERROR_SUCCESS) return status;
    status = RegSetValueExW(handle, name, 0, type, static_cast<const BYTE*>(data), bytes);
    RegCloseKey(handle);
    return status;
}

inline bool autoSaveEnabled(const wchar_t* key = settingsKey) {
    DWORD enabled = 1;
    DWORD bytes = sizeof(enabled);
    return RegGetValueW(HKEY_CURRENT_USER, key, autoSaveValue, RRF_RT_REG_DWORD, nullptr, &enabled, &bytes)
        != ERROR_SUCCESS || enabled != 0;
}

inline Session loadSession(const wchar_t* key = settingsKey) {
    if (!autoSaveEnabled(key)) return {};
    DWORD bytes{};
    if (RegGetValueW(HKEY_CURRENT_USER, key, sessionValue, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t) || bytes > maxSessionBytes || bytes % sizeof(wchar_t)) return {};
    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 1);
    bytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, key, sessionValue, RRF_RT_REG_SZ, nullptr, data.data(), &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t) || bytes > maxSessionBytes || bytes % sizeof(wchar_t) || data[bytes / sizeof(wchar_t) - 1]) return {};
    return parseSession({data.data(), bytes / sizeof(wchar_t) - 1}).value_or(Session{});
}

inline LSTATUS saveSession(const Session& session, const wchar_t* key = settingsKey) {
    if (session.sources.size() > maxSources || session.outputId.size() > maxTargetLength) return ERROR_INVALID_DATA;
    for (const auto& source : session.sources)
        if (source.target.size() > maxTargetLength) return ERROR_INVALID_DATA;
    const auto value = serializeSession(session);
    if (!parseSession(value)) return ERROR_INVALID_DATA;
    return writeSessionValue(key, sessionValue, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

inline LSTATUS clearSession(const wchar_t* key = settingsKey) {
    HKEY handle{};
    auto status = RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE, &handle);
    if (status == ERROR_SUCCESS) {
        status = RegDeleteValueW(handle, sessionValue);
        RegCloseKey(handle);
    }
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

inline LSTATUS setAutoSaveEnabled(bool enabled, const wchar_t* key = settingsKey) {
    const DWORD value = enabled ? 1 : 0;
    return writeSessionValue(key, autoSaveValue, REG_DWORD, &value, sizeof(value));
}
}
