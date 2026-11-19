#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "audio.h"
#include "assets/resource.h"
#include "virtual_device.h"
#include "language.h"
#include "settings.h"
#include "source.h"
#include "session.h"

namespace {
using micpatch::tr;
constexpr wchar_t kWindowClass[] = L"MicPatch.MainWindow";
constexpr auto& kVirtualMicOutputId = micpatch::virtualMicId;
constexpr int kMinWidth = 740;
constexpr int kMinHeight = 632;
constexpr UINT_PTR kPollTimer = 1, kSaveTimer = 2;
constexpr UINT kSaveDelay = 500;
constexpr size_t kNoSource = micpatch::maxSources;
using micpatch::SourceKind;
constexpr int kMuteHotkey = 1;
constexpr UINT kDpiChanged = 0x02E0;  // WM_DPICHANGED
constexpr UINT kVirtualInstallerFinished = WM_APP + 42;

enum ControlId {
    Add = 100, Refresh, SourceDevice, RemoveSource, ChooseFile, OutputDevice, OutMute,
    Start, Stop, AddMicMenu, AddFileMenu, AddLoopbackMenu, VoiceEq, Ducking, VirtualDevice,
    OpenSettings, ScrollSources, FirstSourceControl = 300
};
enum RowControl { SelectRow, GainRow, MuteRow, PlayRow, LoopRow, RowControlCount };
constexpr int rowId(size_t index, RowControl control) {
    return FirstSourceControl + static_cast<int>(index) * RowControlCount + control;
}

struct DpiApi {
    HMODULE user = GetModuleHandleW(L"user32.dll");
    using SetContext = BOOL (WINAPI*)(HANDLE);
    using SetAware = BOOL (WINAPI*)();
    using GetWindowDpi = UINT (WINAPI*)(HWND);
    using GetSystemDpi = UINT (WINAPI*)();
    using GetMetrics = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    using AdjustRect = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);

    template <class Fn> Fn get(const char* name) const {
        return reinterpret_cast<Fn>(GetProcAddress(user, name));
    }

    void enable() const {
        if (const auto set = get<SetContext>("SetProcessDpiAwarenessContext")) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            if (set(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return;
        }
        if (const auto set = get<SetAware>("SetProcessDPIAware")) set();
    }

    UINT system() const {
        if (const auto getDpi = get<GetSystemDpi>("GetDpiForSystem")) {
            if (const UINT dpi = getDpi()) return dpi;
        }
        if (const HDC dc = GetDC(nullptr)) {
            const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(nullptr, dc);
            if (dpi > 0) return static_cast<UINT>(dpi);
        }
        return 96;
    }

    UINT window(HWND hwnd) const {
        if (const auto getDpi = get<GetWindowDpi>("GetDpiForWindow")) {
            if (const UINT dpi = getDpi(hwnd)) return dpi;
        }
        return system();
    }

    void adjust(RECT& r, DWORD style, DWORD exStyle, UINT dpi) const {
        if (const auto adjustForDpi = get<AdjustRect>("AdjustWindowRectExForDpi")) {
            if (adjustForDpi(&r, style, FALSE, exStyle, dpi)) return;
        }
        AdjustWindowRectEx(&r, style, FALSE, exStyle);
    }
};

constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
constexpr DWORD kWindowExStyle = WS_EX_CONTROLPARENT;
const DpiApi kDpi;

struct UiApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~UiApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

constexpr const wchar_t* muteHotkeyName(micpatch::MuteHotkey choice) {
    switch (choice) {
    case micpatch::MuteHotkey::Standard: return L"Ctrl+Alt+M";
    case micpatch::MuteHotkey::Alternate: return L"Ctrl+Alt+Shift+M";
    default: return L"";
    }
}

constexpr UINT muteHotkeyModifiers(micpatch::MuteHotkey choice) {
    return MOD_CONTROL | MOD_ALT | (choice == micpatch::MuteHotkey::Alternate ? MOD_SHIFT : 0);
}

#include "mixer_ui.h"

    static const micpatch::Endpoint* findEndpoint(const std::vector<micpatch::Endpoint>& list,
                                                   const std::wstring& id) {
        if (id.empty()) return nullptr;
        const auto it = std::find_if(list.begin(), list.end(), [&](const auto& entry) { return entry.id == id; });
        return it == list.end() ? nullptr : &*it;
    }

    static constexpr LRESULT kMissingItem = -2;
    static bool hasMissingSelection(HWND comboBox) {
        const auto row = SendMessageW(comboBox, CB_GETCURSEL, 0, 0);
        return row != CB_ERR && SendMessageW(comboBox, CB_GETITEMDATA, row, 0) == kMissingItem;
    }

    static void removeMissingItem(HWND comboBox) {
        const auto count = SendMessageW(comboBox, CB_GETCOUNT, 0, 0);
        // The placeholder is appended after all regular choices.
        if (count > 1 && SendMessageW(comboBox, CB_GETITEMDATA, count - 1, 0) == kMissingItem)
            SendMessageW(comboBox, CB_DELETESTRING, count - 1, 0);
    }

    static std::wstring selectedId(HWND comboBox, const std::vector<micpatch::Endpoint>& list) {
        const LRESULT row = SendMessageW(comboBox, CB_GETCURSEL, 0, 0);
        if (row == CB_ERR) return {};
        const LRESULT data = SendMessageW(comboBox, CB_GETITEMDATA, row, 0);
        if (data <= 0 || static_cast<size_t>(data) > list.size()) return {};
        return list[static_cast<size_t>(data) - 1].id;
    }

    static void populate(HWND comboBox, const std::vector<micpatch::Endpoint>& list,
                         const std::wstring& keep, const wchar_t* prompt) {
        SendMessageW(comboBox, CB_RESETCONTENT, 0, 0);
        LRESULT selection = SendMessageW(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(prompt));
        const auto placeholder = selection;
        int dropWidth = 0;
        const HDC dc = GetDC(comboBox);
        const HFONT font = reinterpret_cast<HFONT>(SendMessageW(comboBox, WM_GETFONT, 0, 0));
        const HGDIOBJ old = dc && font ? SelectObject(dc, font) : nullptr;
        for (size_t i = 0; i < list.size(); ++i) {
            const auto& endpoint = list[i];
            const LRESULT row = SendMessageW(comboBox, CB_ADDSTRING, 0,
                                             reinterpret_cast<LPARAM>(endpoint.name.c_str()));
            if (row < 0) continue;
            SendMessageW(comboBox, CB_SETITEMDATA, row, static_cast<LPARAM>(i + 1));
            if (!keep.empty() && endpoint.id == keep) selection = row;
            SIZE extent{};
            if (dc && GetTextExtentPoint32W(dc, endpoint.name.c_str(),
                                            static_cast<int>(endpoint.name.size()), &extent)) {
                dropWidth = std::max(dropWidth, static_cast<int>(extent.cx));
            }
        }
        if (!keep.empty() && selection == placeholder) {
            const auto missing = tr(L"设备不可用（保留原选择）", L"Device unavailable (selection kept)");
            const auto row = SendMessageW(comboBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(missing));
            if (row >= 0) {
                SendMessageW(comboBox, CB_SETITEMDATA, row, kMissingItem);
                selection = row;
                SIZE extent{};
                if (dc && GetTextExtentPoint32W(dc, missing, lstrlenW(missing), &extent))
                    dropWidth = std::max(dropWidth, static_cast<int>(extent.cx));
            }
        }
        if (old) SelectObject(dc, old);
        if (dc) ReleaseDC(comboBox, dc);
        SendMessageW(comboBox, CB_SETDROPPEDWIDTH, dropWidth + 32, 0);
        SendMessageW(comboBox, CB_SETCURSEL, selection, 0);
    }

    void showStatus(const std::wstring& text, bool error = false) {
        if (statusError == error && statusText == text) return;
        statusError = error;
        statusText = text;
        const std::wstring shown = error ? tr(L"错误：", L"Error: ") + text : text;
        SetWindowTextW(status, shown.c_str());
        InvalidateRect(status, nullptr, TRUE);
    }

    void showOutputStatus(std::wstring text) {
        if (outputId == kVirtualMicOutputId) {
            text += tr(L"\n请在通话软件中选择麦克风（声搭），无需虚拟音频线。", L"\nChoose the MicPatch microphone in your call app. No virtual cable is needed.");
        }
        showStatus(text);
    }

    static const wchar_t* sourceKindName(SourceKind kind) {
        switch (kind) {
        case SourceKind::Microphone: return tr(L"麦克风", L"Microphone");
        case SourceKind::File: return tr(L"本地文件", L"Local File");
        case SourceKind::Loopback: return tr(L"播放回环", L"Playback Loopback");
        }
        return L"";
    }

    const std::vector<micpatch::Endpoint>& inputDevices(SourceKind kind) const {
        return kind == SourceKind::Microphone ? microphones : loopbackDevices;
    }

    std::wstring sourceName(size_t i) const {
        const auto& source = sources[i];
        if (source.kind == SourceKind::File && !source.target.empty())
            return source.target.substr(source.target.find_last_of(L"\\/") + 1);
        return sourceKindName(source.kind);
    }

    std::wstring sourceDescription(size_t i) const {
        const auto& source = sources[i];
        if (source.target.empty()) return tr(L"尚未选择来源", L"No source selected");
        if (rows[i].missing) return source.kind == SourceKind::File
            ? tr(L"文件缺失或不可访问", L"File missing or inaccessible") : tr(L"设备缺失", L"Device missing");
        if (rows[i].failed) return tr(L"音源运行失败", L"Source failed");
        if (source.kind == SourceKind::File) return tr(L"本地音频文件", L"Local audio file");
        if (const auto* device = findEndpoint(inputDevices(source.kind), source.target)) return device->name;
        return sourceKindName(source.kind);
    }

    bool loopbackConflict() const {
        return !outputId.empty() && std::any_of(sources.begin(), sources.end(), [&](const auto& source) {
            return source.kind == SourceKind::Loopback && !source.target.empty() &&
                   CompareStringOrdinal(source.target.c_str(), -1, outputId.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
    }

    bool hasAvailableSource() const {
        for (size_t i = 0; i < sources.size(); ++i) if (!rows[i].missing) return true;
        return false;
    }

    void idleStatus() {
        if (!outputId.empty() && !findEndpoint(outputs, outputId))
            showStatus(tr(L"所选输出设备缺失；原选择已保留。请重新连接并刷新。", L"The selected output device is missing. Its selection is kept; reconnect it and refresh."));
        else if (outputs.empty()) showStatus(tr(L"未找到输出设备，请连接设备后刷新。", L"No output devices found. Connect a device and refresh."));
        else if (outputId.empty()) showStatus(tr(L"请选择输出设备。不会自动使用默认设备。", L"Select an output device. The system default is not selected automatically."));
        else if (loopbackConflict()) showStatus(tr(L"播放回环不能采集发送所用的同一设备。请选择不同的设备。", L"Playback loopback cannot capture the sending output. Choose different devices."), true);
        else if (sources.empty()) showOutputStatus(tr(L"请添加至少一个音源。", L"Add at least one audio source."));
        else if (!hasAvailableSource()) showOutputStatus(tr(L"没有可用的音源，请检查设备或文件。", L"No source is available. Check the devices or files."));
        else if (std::any_of(sources.begin(), sources.end(), [](const auto& source) { return source.kind == SourceKind::Loopback; }))
            showOutputStatus(tr(L"回环会捕获所选设备的全部播放声音，包括通话对方。", L"Loopback captures all playback on the selected device, including callers."));
        else showOutputStatus(tr(L"就绪。启动后可手动播放文件。", L"Ready. Start sending, then press Play to play a file."));
    }

    void checkSources() {
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto& source = sources[i];
            if (source.kind == SourceKind::File && !source.target.empty()) {
                const auto attr = GetFileAttributesW(source.target.c_str());
                rows[i].missing = attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
            } else {
                rows[i].missing = !findEndpoint(inputDevices(source.kind), source.target);
            }
        }
    }

    bool refreshDevices(bool updateStatus = true) {
        if (active) return false;
        try {
            auto nextMic = micpatch::endpoints(true);
            auto nextOut = micpatch::endpoints(false);
            auto nextLoopback = nextOut;
            std::erase_if(nextLoopback, [](const auto& entry) { return entry.id == kVirtualMicOutputId; });
            microphones = std::move(nextMic);
            outputs = std::move(nextOut);
            loopbackDevices = std::move(nextLoopback);
        } catch (...) {
            checkSources();
            updateControls();
            if (updateStatus) showStatus(tr(L"无法枚举音频设备，请重试。", L"Could not list audio devices. Please try again."), true);
            return false;
        }
        populate(outputDevice, outputs, outputId,
                 outputs.empty() ? tr(L"没有可用的输出设备", L"No output devices available") : tr(L"请选择输出设备", L"Select an output device"));
        for (auto& row : rows) row.failed = false;
        checkSources();
        updateControls();
        if (updateStatus) idleStatus();
        return true;
    }

    void showVirtualDevice() {
        const auto title = tr(L"虚拟麦克风", L"Virtual Microphone");
        const auto device = micpatch::virtualDeviceStatus();
        if (active || !device.canInstall) {
            auto message = device.detail;
            if (active) message += tr(L"\n\n运行中仅可查看；停止发送后才能安装。", L"\n\nStop sending before installing the driver.");
            MessageBoxW(window, message.c_str(), title, MB_OK | MB_ICONINFORMATION);
            return;
        }
        const auto message = device.detail + tr(L"\n\n是否安装虚拟麦克风？安装将请求管理员权限。", L"\n\nInstall the virtual microphone? Installation requires administrator privileges.");
        if (MessageBoxW(window, message.c_str(), title, MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES ||
            active || !IsWindow(window)) return;
        if (!micpatch::launchVirtualInstaller(window)) {
            MessageBoxW(window, tr(L"未启动安装程序。", L"The installer did not start."), title, MB_OK | MB_ICONWARNING);
        }
    }

    void virtualInstallerFinished() {
        if (active) {
            // Keep the selected endpoints stable until the current session ends.
            virtualRefreshPending = true;
        } else if (refreshDevices()) {
            showOutputStatus(tr(L"设备列表已刷新。可打开“虚拟麦克风”复检。", L"Device list refreshed. Open Virtual Mic to check its status."));
        }
    }

    bool hasStartConfig() const {
        return findEndpoint(outputs, outputId) && hasAvailableSource() && !loopbackConflict();
    }

    micpatch::SourceControls* liveSource(size_t index) {
        return active ? engine.source(index) : nullptr;
    }

    void updatePlay(size_t index, const micpatch::SourceControls* state) const {
        const bool canPlay = state && engine.running() && !rows[index].missing && !state->failed.load();
        const auto hwnd = rows[index].play;
        if ((IsWindowEnabled(hwnd) != FALSE) != canPlay) EnableWindow(hwnd, canPlay);
        const auto title = canPlay && state->playing.load() ? tr(L"暂停", L"Pause") : tr(L"播放", L"Play");
        if (caption(hwnd) != title) SetWindowTextW(hwnd, title);
    }

    void updateControls(bool updatePicker = true) {
        const bool selected = detail < sources.size();
        const auto kind = selected ? sources[detail].kind : SourceKind::Microphone;
        const bool file = selected && kind == SourceKind::File;
        if (tooltip) SendMessageW(tooltip, TTM_POP, 0, 0);
        showControl(sourceDevice, selected && !file);
        showControl(filePath, file);
        showControl(chooseFileButton, file);
        showControl(voiceEq, selected && kind == SourceKind::Microphone);
        showControl(ducking, file);
        showControl(removeSource, selected);
        if (file) {
            SetWindowTextW(filePath, sources[detail].target.c_str());
        } else if (selected && updatePicker) {
            populate(sourceDevice, inputDevices(kind), sources[detail].target,
                     kind == SourceKind::Loopback ? tr(L"请选择播放设备", L"Select a playback device")
                                                  : tr(L"请选择录音设备", L"Select a recording device"));
        }
        EnableWindow(add, !active && sources.size() < rows.size());
        EnableWindow(refresh, !active);
        EnableWindow(sourceDevice, !active && selected && !file);
        EnableWindow(chooseFileButton, !active && file);
        EnableWindow(removeSource, !active && selected);
        EnableWindow(outputDevice, !active);
        EnableWindow(start, !active && hasStartConfig());
        EnableWindow(stop, active);
        for (size_t i = 0; i < sources.size(); ++i) {
            auto& row = rows[i];
            const auto title = sourceName(i) + L" — " + sourceDescription(i);
            if (caption(row.select) != title) SetWindowTextW(row.select, title.c_str());
            SendMessageW(row.gain, TBM_SETPOS, TRUE, sources[i].gain);
            if (sources[i].kind == SourceKind::File) updatePlay(i, liveSource(i));
        }
        layout();
    }

    void revealSource(size_t index) {
        const int pos = static_cast<int>(index);
        if (pos < firstRow) firstRow = pos;
        else if (pos >= firstRow + rowPage) firstRow = pos - rowPage + 1;
    }

    void scrollTo(int pos) {
        pos = std::clamp(pos, 0, std::max(0, static_cast<int>(sources.size()) - rowPage));
        if (pos == firstRow) return;
        firstRow = pos;
        layout();
    }

    void scrollSources(int code) {
        switch (code) {
        case SB_LINEUP: scrollTo(firstRow - 1); break;
        case SB_LINEDOWN: scrollTo(firstRow + 1); break;
        case SB_PAGEUP: scrollTo(firstRow - rowPage); break;
        case SB_PAGEDOWN: scrollTo(firstRow + rowPage); break;
        case SB_TOP: scrollTo(0); break;
        case SB_BOTTOM: scrollTo(static_cast<int>(sources.size())); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: {
            SCROLLINFO info{sizeof(SCROLLINFO), SIF_TRACKPOS};
            if (GetScrollInfo(sourceScroll, SB_CTL, &info)) scrollTo(info.nTrackPos);
            break;
        }
        }
    }

    bool flushSession() {
        KillTimer(window, kSaveTimer);
        if (!autoSave || !saveDirty) return true;
        saveError = micpatch::saveSession({preferences.rememberOutput ? outputId : std::wstring{}, sources});
        if (saveError == ERROR_SUCCESS) saveDirty = false;
        InvalidateRect(window, &footer, FALSE);
        return saveError == ERROR_SUCCESS;
    }

    void queueSave() {
        if (!autoSave) return;
        saveDirty = true;
        saveAfter = GetTickCount64() + kSaveDelay;
        if (!SetTimer(window, kSaveTimer, kSaveDelay, nullptr)) flushSession();
    }

    void saveTimerElapsed() {
        // A queued WM_TIMER from before the debounce was reset can arrive early.
        if (GetTickCount64() >= saveAfter) flushSession();
    }

    void configurationChanged(bool updatePicker = true) {
        checkSources();
        updateControls(updatePicker);
        idleStatus();
        queueSave();
    }

    void appendSource(SourceKind kind, std::wstring target = {}) {
        if (active || sources.size() >= rows.size()) return;
        const size_t index = sources.size();
        sources.push_back({kind, std::move(target)});
        rows[index].peak = 0;
        rows[index].failed = false;
        detail = index;
        revealSource(index);
        configurationChanged();
        SetFocus(kind == SourceKind::File ? rows[index].select : sourceDevice);
    }

    void chooseFile(size_t replace = kNoSource) {
        if (active || (replace == kNoSource && sources.size() >= rows.size())) return;
        std::vector<wchar_t> name(32768, L'\0');
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window;
        dialog.lpstrTitle = tr(L"选择本地音频文件", L"Choose a local audio file");
        dialog.lpstrFilter = tr(L"所有文件\0*.*\0", L"All files\0*.*\0");
        dialog.lpstrFile = name.data();
        dialog.nMaxFile = static_cast<DWORD>(name.size());
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog)) {
            if (active) return;
            if (replace == kNoSource) appendSource(SourceKind::File, name.data());
            else if (replace < sources.size() && sources[replace].kind == SourceKind::File) {
                sources[replace].target = name.data();
                rows[replace].failed = false;
                configurationChanged();
            }
        } else if (const DWORD error = CommDlgExtendedError()) {
            showStatus(tr(L"无法打开文件选择窗口（代码 ", L"Could not open the file picker (error ") + std::to_wstring(error) + tr(L"）。", L")."), true);
        }
    }

    void addMenu() {
        if (active || sources.size() >= rows.size()) return;
        const HMENU menu = CreatePopupMenu();
        if (!menu) {
            showStatus(tr(L"无法打开添加音源菜单。", L"Could not open the Add Source menu."), true);
            return;
        }
        AppendMenuW(menu, MF_STRING, AddMicMenu, tr(L"添加麦克风", L"Add Microphone"));
        AppendMenuW(menu, MF_STRING, AddFileMenu, tr(L"添加本地文件…", L"Add Local File…"));
        AppendMenuW(menu, MF_STRING, AddLoopbackMenu, tr(L"添加播放回环", L"Add Playback Loopback"));
        RECT anchor{};
        GetWindowRect(add, &anchor);
        const UINT choice = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD |
                                           TPM_NONOTIFY, anchor.left, anchor.bottom, 0, window, nullptr);
        DestroyMenu(menu);
        if (choice == AddMicMenu) appendSource(SourceKind::Microphone);
        else if (choice == AddFileMenu) chooseFile();
        else if (choice == AddLoopbackMenu) appendSource(SourceKind::Loopback);
    }

    static int level(float peak) {
        if (!std::isfinite(peak)) return 0;
        return static_cast<int>(std::lround(std::clamp(peak, 0.0f, 1.0f) * 10000.0f));
    }

    void updateMeters() {
        const auto update = [&](int& old, int next, const RECT& bounds, bool visible) {
            if (old == next) return;
            old = next;
            if (visible) InvalidateRect(window, &bounds, FALSE);
        };
        for (size_t i = 0; i < sources.size(); ++i) {
            auto& row = rows[i];
            const auto* state = liveSource(i);
            if (state) {
                const bool failed = state->failed.load();
                if (failed != row.failed) {
                    row.failed = failed;
                    const auto title = sourceName(i) + L" — " + sourceDescription(i);
                    SetWindowTextW(row.select, title.c_str());
                    if (row.visible) InvalidateRect(row.select, nullptr, FALSE);
                    if (detail == i) InvalidateRect(window, &inspector, FALSE);
                }
            }
            update(row.peak, state && !row.failed ? level(state->peak.load()) : 0, row.meter, row.visible);
            if (sources[i].kind == SourceKind::File) updatePlay(i, state);
        }
        update(outPeak, active ? level(engine.outPeak.load()) : 0, master, true);
    }

    void begin() {
        if (active) return;
        checkSources();
        if (!hasStartConfig()) {
            updateControls();
            idleStatus();
            return;
        }
        for (auto& row : rows) row.failed = false;
        engine.outPeak.store(0.0f);
        const auto failure = engine.start(outputId, sources);
        if (!failure.empty()) {
            updateControls();
            showStatus(failure, true);
            return;
        }
        active = true;
        starting = !engine.running();
        startTick = GetTickCount64();
        const auto focus = GetFocus();
        updateControls();
        if (!focus || !IsWindowEnabled(focus)) SetFocus(stop);
        if (starting) showOutputStatus(tr(L"正在启动…", L"Starting…"));
        else runningStatus();
    }

    void runningStatus() {
        const bool hasFile = std::any_of(sources.begin(), sources.end(), [](const auto& source) { return source.kind == SourceKind::File; });
        showOutputStatus(hasFile ? tr(L"运行中。点击对应的“播放”按钮开始播放文件。", L"Sending. Press Play on a file to play it.")
                                 : tr(L"运行中。", L"Sending."));
    }

    void end(bool stoppedByUser = false) {
        const auto focus = GetFocus();
        updateMeters();
        engine.stop();
        active = false;
        starting = false;
        flushSession();
        if (std::exchange(virtualRefreshPending, false)) {
            if (stoppedByUser) virtualInstallerFinished();
            else refreshDevices(false);  // Preserve the engine error on an automatic stop.
        } else {
            checkSources();
            updateControls();
            if (stoppedByUser) showOutputStatus(tr(L"已停止。可重新选择设备或音源。", L"Stopped. You can change devices or sources."));
        }
        updateMeters();
        if (!focus || !IsWindowEnabled(focus)) {
            SetFocus(IsWindowEnabled(start) ? start : IsWindowEnabled(add) ? add : refresh);
        }
    }

    void poll() {
        if (!active) return;
        const bool running = engine.running();
        const auto failure = engine.outputError();
        if (!failure.empty()) {
            showStatus(failure, true);
            end();
            return;
        }
        updateMeters();
        if (!running) {
            if (starting && GetTickCount64() - startTick < 5000) return;
            showStatus(starting ? tr(L"音频引擎启动超时，请检查设备后重试。", L"Audio engine timed out. Check your devices and try again.")
                                : tr(L"音频引擎已停止，请检查输出设备后重试。", L"The audio engine stopped. Check the output device and try again."), true);
            end();
            return;
        }
        if (starting) {
            starting = false;
            InvalidateRect(window, &banner, FALSE);
            runningStatus();
        }
    }

    bool isGain(HWND slider) const {
        if (!slider) return false;
        const int id = GetDlgCtrlID(slider);
        const auto index = controlRow(id);
        return index < rows.size() && rowAction(id) == GainRow && rows[index].gain == slider;
    }

    void gainChanged(HWND slider) {
        if (!isGain(slider)) return;
        const auto index = controlRow(GetDlgCtrlID(slider));
        if (index >= sources.size()) return;
        const int value = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        if (sources[index].gain == value) return;
        sources[index].gain = value;
        if (auto* state = liveSource(index)) state->gain.store(value / 100.0f);
        InvalidateRect(window, &rows[index].value, FALSE);
        queueSave();
    }

    void registerMuteHotkey() {
        muteHotkeyError = ERROR_SUCCESS;
        if (preferences.hotkey != micpatch::MuteHotkey::Off) {
            const UINT mods = muteHotkeyModifiers(preferences.hotkey) | MOD_NOREPEAT;
            muteHotkeyRegistered = RegisterHotKey(window, kMuteHotkey, mods, 'M') != FALSE;
            if (!muteHotkeyRegistered) {
                muteHotkeyError = GetLastError();
                const auto log = std::wstring(L"MicPatch: RegisterHotKey(") + muteHotkeyName(preferences.hotkey) +
                                 L") failed, GetLastError=" + std::to_wstring(muteHotkeyError) + L"\n";
                OutputDebugStringW(log.c_str());
            }
        }
        InvalidateRect(window, &footer, FALSE);
    }

    void unregisterMuteHotkey() {
        if (!muteHotkeyRegistered) return;
        UnregisterHotKey(window, kMuteHotkey);
        muteHotkeyRegistered = false;
    }

    void setOutputMute(bool muted) {
        engine.outMute.store(muted);
        SetWindowTextW(outMute, muted ? tr(L"取消总静音", L"Unmute All") : tr(L"总静音", L"Mute All"));
        InvalidateRect(outMute, nullptr, FALSE);
        InvalidateRect(window, &banner, FALSE);
    }

#include "settings_ui.h"

    void command(int id, int notification) {
        if (id == SourceDevice || id == OutputDevice) {
            if (active || notification != CBN_SELCHANGE) return;
            if (id == OutputDevice) {
                if (hasMissingSelection(outputDevice)) return;
                auto nextId = selectedId(outputDevice, outputs);
                if (nextId == outputId) return;
                outputId = std::move(nextId);
                removeMissingItem(outputDevice);
                if (preferences.rememberOutput && preferences.outputId != outputId) {
                    auto next = preferences;
                    next.outputId = outputId;
                    if (!storePreferences(std::move(next), window)) preferences.outputId = outputId;
                }
                updateControls();
                idleStatus();
                if (preferences.rememberOutput) queueSave();
            } else if (detail < sources.size() && sources[detail].kind != SourceKind::File) {
                if (hasMissingSelection(sourceDevice)) return;
                auto& source = sources[detail];
                auto nextId = selectedId(sourceDevice, inputDevices(source.kind));
                if (nextId == source.target) return;
                source.target = std::move(nextId);
                removeMissingItem(sourceDevice);
                rows[detail].failed = false;
                configurationChanged(false);
            }
            return;
        }
        if (notification != BN_CLICKED) return;
        const auto index = controlRow(id);
        if (index < sources.size() && rows[index].visible) {
            auto& source = sources[index];
            auto* state = liveSource(index);
            switch (rowAction(id)) {
            case SelectRow:
                detail = index;
                updateControls();
                break;
            case MuteRow:
                source.mute = !source.mute;
                if (state) state->mute.store(source.mute);
                InvalidateRect(rows[index].mute, nullptr, FALSE);
                queueSave();
                break;
            case LoopRow:
                if (source.kind != SourceKind::File) break;
                source.loop = !source.loop;
                if (state) state->loop.store(source.loop);
                InvalidateRect(rows[index].loop, nullptr, FALSE);
                queueSave();
                break;
            case PlayRow:
                if (source.kind == SourceKind::File && state && engine.running() && !state->failed.load() && !rows[index].missing) {
                    bool wasPlaying = state->playing.load();
                    while (!state->playing.compare_exchange_weak(wasPlaying, !wasPlaying)) {}
                    updatePlay(index, state);
                }
                break;
            }
            return;
        }
        switch (id) {
        case Add: addMenu(); break;
        case Refresh: refreshDevices(); break;
        case VirtualDevice: showVirtualDevice(); break;
        case OpenSettings: showSettings(); break;
        case ChooseFile:
            if (detail < sources.size() && sources[detail].kind == SourceKind::File) chooseFile(detail);
            break;
        case RemoveSource:
            if (!active && detail < sources.size()) {
                const auto removed = detail;
                sources.erase(sources.begin() + static_cast<std::ptrdiff_t>(removed));
                for (size_t i = removed; i < sources.size(); ++i) rows[i].failed = rows[i + 1].failed;
                detail = sources.empty() ? kNoSource : std::min(removed, sources.size() - 1);
                if (detail != kNoSource) revealSource(detail);
                configurationChanged();
                SetFocus(detail == kNoSource ? add : rows[detail].select);
            }
            break;
        case OutMute: setOutputMute(!engine.outMute.load()); break;
        case VoiceEq:
            if (detail < sources.size() && sources[detail].kind == SourceKind::Microphone) {
                auto& value = sources[detail].voiceEq;
                value = !value;
                if (auto* state = liveSource(detail)) state->voiceEq.store(value);
                InvalidateRect(voiceEq, nullptr, FALSE);
                queueSave();
            }
            break;
        case Ducking:
            if (detail < sources.size() && sources[detail].kind == SourceKind::File) {
                auto& value = sources[detail].ducking;
                value = !value;
                if (auto* state = liveSource(detail)) state->ducking.store(value);
                InvalidateRect(ducking, nullptr, FALSE);
                queueSave();
            }
            break;
        case Start: begin(); break;
        case Stop: if (active) end(true); break;
        }
    }

};

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* app = static_cast<App*>(create->lpCreateParams);
        app->window = window;
        app->dpi = kDpi.window(window);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!app) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE:
        if (!app->createControls()) return -1;
        app->refreshDevices();
        if (!SetTimer(window, kPollTimer, 80, nullptr)) return -1;
        app->registerMuteHotkey();
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) app->layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT bounds{0, 0, app->px(kMinWidth), app->px(kMinHeight)};
        kDpi.adjust(bounds, kWindowStyle, kWindowExStyle, app->dpi);
        info->ptMinTrackSize.x = bounds.right - bounds.left;
        info->ptMinTrackSize.y = bounds.bottom - bounds.top;
        return 0;
    }
    case kDpiChanged: {
        app->dpi = HIWORD(wParam);
        app->setFont();
        const auto& suggested = *reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested.left, suggested.top,
                     suggested.right - suggested.left, suggested.bottom - suggested.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        app->layout();
        return 0;
    }
    case WM_COMMAND:
        app->command(LOWORD(wParam), HIWORD(wParam));
        return 0;
    case kVirtualInstallerFinished:
        app->virtualInstallerFinished();
        return 0;
    case WM_HOTKEY:
        if (wParam == kMuteHotkey && app->muteHotkeyRegistered && HIWORD(lParam) == 'M' &&
            (static_cast<UINT>(LOWORD(lParam)) & (MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_WIN)) ==
                muteHotkeyModifiers(app->preferences.hotkey)) {
            app->setOutputMute(!app->engine.outMute.load());
            return 0;
        }
        break;
    case WM_VSCROLL:
        if (reinterpret_cast<HWND>(lParam) == app->sourceScroll) app->scrollSources(LOWORD(wParam));
        return 0;
    case WM_MOUSEWHEEL: {
        POINT cursor{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        ScreenToClient(window, &cursor);
        if (cursor.x >= app->px(20) && cursor.x < app->inspector.left &&
            cursor.y >= app->px(229) && cursor.y < app->footer.top &&
            static_cast<int>(app->sources.size()) > app->rowPage) {
            app->wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
            app->scrollTo(app->firstRow - app->wheelRemainder / WHEEL_DELTA);
            app->wheelRemainder %= WHEEL_DELTA;
            return 0;
        }
        break;
    }
    case WM_HSCROLL:
        app->gainChanged(reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_TIMER:
        if (wParam == kPollTimer) app->poll();
        else if (wParam == kSaveTimer) app->saveTimerElapsed();
        return 0;
    case WM_NOTIFY: {
        auto* notification = reinterpret_cast<NMHDR*>(lParam);
        if (notification->code == NM_CUSTOMDRAW &&
            app->isGain(notification->hwndFrom))
            return app->drawSlider(*reinterpret_cast<NMCUSTOMDRAW*>(notification));
        if (notification->hwndFrom == app->tooltip && notification->code == TTN_GETDISPINFOW) {
            auto* tip = reinterpret_cast<NMTTDISPINFOW*>(notification);
            const auto index = app->detail;
            tip->szText[0] = L'\0';
            if (index < app->sources.size() && app->sources[index].kind == SourceKind::File)
                tip->lpszText = app->sources[index].target.data();
            else tip->lpszText = tip->szText;
            return 0;
        }
        break;
    }
    case WM_PAINT: app->paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item.CtlType == ODT_BUTTON) { app->drawButton(item); return TRUE; }
        if (item.CtlType == ODT_COMBOBOX) { app->drawComboItem(item); return TRUE; }
        break;
    }
    case WM_MEASUREITEM: {
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (item.CtlType == ODT_COMBOBOX) { item.itemHeight = app->px(27); return TRUE; }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX: {
        const auto dc = reinterpret_cast<HDC>(wParam);
        return reinterpret_cast<LRESULT>(app->controlColors(dc, reinterpret_cast<HWND>(lParam),
                                                            message == WM_CTLCOLORLISTBOX));
    }
    case WM_CLOSE:
        if (!app->flushSession()) {
            const auto prompt = tr(L"无法保存上次会话（错误 ", L"Could not save the last session (error ") +
                                std::to_wstring(app->saveError) + tr(L"）。仍然退出？", L"). Exit anyway?");
            if (MessageBoxW(window, prompt.c_str(), tr(L"声搭", L"MicPatch"), MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES)
                return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_ENDSESSION:
        if (wParam) app->flushSession();
        return 0;
    case WM_DESTROY:
        app->flushSession();
        app->unregisterMuteHotkey();
        KillTimer(window, kPollTimer);
        if (app->active) app->engine.stop();
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    kDpi.enable();
    const INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES};
    if (!InitCommonControlsEx(&common)) return 1;

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_MICPATCH));
    cls.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_MICPATCH), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = nullptr;
    cls.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&cls)) return 1;

    const UiApartment apartment;
    if (FAILED(apartment.result)) return 1;
    auto preferences = micpatch::loadSettings();
    micpatch::english.store(preferences.english);
    App app(instance, std::move(preferences));
    app.dpi = kDpi.system();
    RECT initial{0, 0, app.px(930), app.px(660)};
    kDpi.adjust(initial, kWindowStyle, kWindowExStyle, app.dpi);
    const HWND window = CreateWindowExW(kWindowExStyle, kWindowClass, tr(L"声搭 · MicPatch", L"MicPatch"),
                                        kWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                                        initial.right - initial.left, initial.bottom - initial.top,
                                        nullptr, nullptr, instance, &app);
    if (!window) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);

    MSG msg{};
    BOOL result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && IsChild(window, msg.hwnd) &&
            (GetWindowLongPtrW(msg.hwnd, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW) {
            wchar_t clsName[16]{};
            if (GetClassNameW(msg.hwnd, clsName, 16) && std::wstring_view(clsName) == L"Button") {
                if (!(msg.lParam & (1L << 30))) SendMessageW(msg.hwnd, BM_CLICK, 0, 0);
                continue;
            }
        }
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (result == -1) {
        DestroyWindow(window);
        return 1;
    }
    return static_cast<int>(msg.wParam);
}
