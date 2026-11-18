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

namespace {
using micpatch::tr;
constexpr wchar_t kWindowClass[] = L"MicPatch.MainWindow";
constexpr auto& kVirtualMicOutputId = micpatch::virtualMicId;
constexpr int kMinWidth = 740;
constexpr int kMinHeight = 632;
constexpr UINT_PTR kPollTimer = 1;
constexpr int kMuteHotkey = 1;
constexpr UINT kDpiChanged = 0x02E0;  // WM_DPICHANGED
constexpr UINT kVirtualInstallerFinished = WM_APP + 42;

enum ControlId {
    Add = 100, Refresh, MicDevice, ClearMic, MicGain, MicMute,
    ClearFile, FileGain, FileMute, Play, Loop, OutputDevice, OutMute,
    Start, Stop, AddMicMenu, AddFileMenu, VoiceEq, Ducking, SelectMic, SelectFile, VirtualDevice, OpenSettings
};

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
        if (selectedId(outputDevice, outputs) == kVirtualMicOutputId) {
            text += tr(L"\n请在通话软件中选择麦克风（声搭），无需虚拟音频线。", L"\nChoose the MicPatch microphone in your call app. No virtual cable is needed.");
        }
        showStatus(text);
    }

    void idleStatus() {
        if (outputs.empty()) showStatus(tr(L"未找到输出设备，请连接设备后刷新。", L"No output devices found. Connect a device and refresh."));
        else if (selectedId(outputDevice, outputs).empty()) showStatus(tr(L"请选择输出设备。不会自动使用默认设备。", L"Select an output device. The system default is not selected automatically."));
        else if (micAdded && selectedId(micDevice, microphones).empty()) {
            showOutputStatus(microphones.empty() ? tr(L"未找到麦克风，请刷新设备或清除麦克风。", L"No microphone found. Refresh devices or remove the microphone source.")
                                                 : tr(L"请选择麦克风，或清除该音源。", L"Select a microphone or remove the source."));
        } else if (!micAdded && path.empty()) showOutputStatus(tr(L"请添加至少一个音源。", L"Add at least one audio source."));
        else showOutputStatus(tr(L"就绪。启动后可手动播放文件。", L"Ready. Start sending, then press Play to play a file."));
    }

    bool refreshDevices(bool updateStatus = true) {
        if (active) return false;
        const auto keepMic = selectedId(micDevice, microphones);
        auto keepOut = selectedId(outputDevice, outputs);
        if (keepOut.empty() && preferences.rememberOutput) keepOut = preferences.outputId;
        try {
            auto nextMic = micpatch::endpoints(true);
            auto nextOut = micpatch::endpoints(false);
            microphones = std::move(nextMic);
            outputs = std::move(nextOut);
        } catch (...) {
            updateControls();
            if (updateStatus) showStatus(tr(L"无法枚举音频设备，请重试。", L"Could not list audio devices. Please try again."), true);
            return false;
        }
        populate(micDevice, microphones, keepMic,
                 microphones.empty() ? tr(L"没有可用的麦克风", L"No microphones available") : tr(L"请选择麦克风", L"Select a microphone"));
        populate(outputDevice, outputs, keepOut,
                 outputs.empty() ? tr(L"没有可用的输出设备", L"No output devices available") : tr(L"请选择输出设备", L"Select an output device"));
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
        return !selectedId(outputDevice, outputs).empty() && (micAdded || !path.empty()) &&
               (!micAdded || !selectedId(micDevice, microphones).empty());
    }

    void updatePlay() const {
        const bool canPlay = active && !path.empty() && engine.running();
        EnableWindow(play, canPlay);
        const wchar_t* caption = canPlay && engine.playing.load() ? tr(L"暂停", L"Pause") : tr(L"播放", L"Play");
        if (App::caption(play) != caption) SetWindowTextW(play, caption);
    }

    void updateControls() {
        const bool hasFile = !path.empty();
        if ((detail == Detail::Mic && !micAdded) || (detail == Detail::File && !hasFile)) detail = Detail::None;
        if (detail == Detail::None) detail = micAdded ? Detail::Mic : hasFile ? Detail::File : Detail::None;
        const bool showMic = detail == Detail::Mic, showFile = detail == Detail::File;
        const auto show = [](HWND control, bool visible) {
            ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
        };
        for (HWND control : {mic.select, mic.gain, mic.mute}) show(control, micAdded);
        for (HWND control : {file.select, file.gain, file.mute, play, loop}) show(control, hasFile);
        for (HWND control : {micDevice, clearMic, voiceEq}) show(control, showMic);
        for (HWND control : {filePath, clearFile, ducking}) show(control, showFile);
        EnableWindow(add, !active && (!micAdded || !hasFile));
        EnableWindow(refresh, !active);
        EnableWindow(micDevice, !active && !microphones.empty());
        EnableWindow(clearMic, !active);
        EnableWindow(clearFile, !active);
        EnableWindow(outputDevice, !active && !outputs.empty());
        EnableWindow(start, !active && hasStartConfig());
        EnableWindow(stop, active);
        updatePlay();
        layout();
    }

    void configurationChanged() {
        updateControls();
        idleStatus();
    }

    void setPath(std::wstring next) {
        if (tooltip) SendMessageW(tooltip, TTM_POP, 0, 0);
        path = std::move(next);
        SetWindowTextW(filePath, path.empty() ? tr(L"尚未添加文件", L"No file added") : path.c_str());
    }

    void chooseFile() {
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
            setPath(name.data());
            detail = Detail::File;
            configurationChanged();
            SetFocus(clearFile);
        } else if (const DWORD error = CommDlgExtendedError()) {
            showStatus(tr(L"无法打开文件选择窗口（代码 ", L"Could not open the file picker (error ") + std::to_wstring(error) + tr(L"）。", L")."), true);
        }
    }

    void addMenu() {
        if (active) return;
        const HMENU menu = CreatePopupMenu();
        if (!menu) {
            showStatus(tr(L"无法打开添加音源菜单。", L"Could not open the Add Source menu."), true);
            return;
        }
        AppendMenuW(menu, MF_STRING | (micAdded ? MF_GRAYED : MF_ENABLED), AddMicMenu, tr(L"添加麦克风", L"Add Microphone"));
        AppendMenuW(menu, MF_STRING | (!path.empty() ? MF_GRAYED : MF_ENABLED), AddFileMenu, tr(L"添加本地文件…", L"Add Local File…"));
        RECT anchor{};
        GetWindowRect(add, &anchor);
        const UINT choice = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD |
                                           TPM_NONOTIFY, anchor.left, anchor.bottom, 0, window, nullptr);
        DestroyMenu(menu);
        if (choice == AddMicMenu && !micAdded) {
            micAdded = true;
            detail = Detail::Mic;
            configurationChanged();
            SetFocus(IsWindowEnabled(micDevice) ? micDevice : clearMic);
        } else if (choice == AddFileMenu && path.empty()) {
            chooseFile();
        }
    }

    static int level(float peak) {
        if (!std::isfinite(peak)) return 0;
        return static_cast<int>(std::lround(std::clamp(peak, 0.0f, 1.0f) * 10000.0f));
    }

    void updateMeters() {
        const auto update = [&](int& old, int next, const RECT& bounds) {
            if (old == next) return;
            old = next;
            InvalidateRect(window, &bounds, FALSE);
        };
        update(mic.peak, active && micAdded ? level(engine.micPeak.load()) : 0, mic.meter);
        update(file.peak, active && !path.empty() ? level(engine.filePeak.load()) : 0, file.meter);
        update(outPeak, active ? level(engine.outPeak.load()) : 0, master);
    }

    void begin() {
        if (active || !hasStartConfig()) return;
        const auto outputId = selectedId(outputDevice, outputs);
        const auto micId = micAdded ? selectedId(micDevice, microphones) : std::wstring{};
        engine.playing.store(false);
        engine.micPeak.store(0.0f);
        engine.filePeak.store(0.0f);
        engine.outPeak.store(0.0f);
        const auto failure = engine.start(outputId, micId, path);
        if (!failure.empty()) {
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
        showOutputStatus(path.empty() ? tr(L"运行中。", L"Sending.") : tr(L"运行中。点击“播放”开始播放文件。", L"Sending. Click Play to play the file."));
    }

    void end(bool stoppedByUser = false) {
        const auto focus = GetFocus();
        engine.stop();
        engine.playing.store(false);
        active = false;
        starting = false;
        if (std::exchange(virtualRefreshPending, false)) {
            if (stoppedByUser) virtualInstallerFinished();
            else refreshDevices(false);  // Preserve the engine error on an automatic stop.
        } else {
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
        const auto failure = engine.error();
        if (!failure.empty()) showStatus(failure, true);
        if (!running) {
            if (starting && !statusError && GetTickCount64() - startTick < 5000) return;
            if (!statusError) {
                showStatus(starting ? tr(L"音频引擎启动超时，请检查设备后重试。", L"Audio engine timed out. Check your devices and try again.")
                                    : tr(L"音频引擎已停止，请检查输出设备后重试。", L"The audio engine stopped. Check the output device and try again."), true);
            }
            end();
            return;
        }
        if (starting) {
            starting = false;
            InvalidateRect(window, &banner, FALSE);
            if (!statusError) runningStatus();
        }
        updateMeters();
        updatePlay();
    }

    void gainChanged(HWND slider) {
        SourceRow* row = slider == mic.gain ? &mic : slider == file.gain ? &file : nullptr;
        if (!row) return;
        const int value = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        (slider == mic.gain ? engine.micGain : engine.fileGain).store(value / 100.0f);
        row->percent = value;
        InvalidateRect(window, &row->value, FALSE);
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
        if (id == MicDevice || id == OutputDevice) {
            if (!active && notification == CBN_SELCHANGE) {
                if (id == OutputDevice && preferences.rememberOutput) {
                    auto next = preferences;
                    next.outputId = selectedId(outputDevice, outputs);
                    if (next.outputId != preferences.outputId && !storePreferences(next, window)) {
                        // Keep the user's selection in this session, even if the write failed.
                        preferences.outputId = std::move(next.outputId);
                    }
                }
                configurationChanged();
            }
            return;
        }
        if (notification != BN_CLICKED) return;
        switch (id) {
        case Add: addMenu(); break;
        case Refresh: refreshDevices(); break;
        case VirtualDevice: showVirtualDevice(); break;
        case OpenSettings: showSettings(); break;
        case ClearMic:
            if (!active && micAdded) {
                micAdded = false;
                SendMessageW(micDevice, CB_SETCURSEL, 0, 0);
                configurationChanged();
                SetFocus(add);
            }
            break;
        case ClearFile:
            if (!active && !path.empty()) {
                setPath({});
                configurationChanged();
                SetFocus(add);
            }
            break;
        case MicMute: engine.micMute.store(!engine.micMute.load()); InvalidateRect(mic.mute, nullptr, FALSE); break;
        case FileMute: engine.fileMute.store(!engine.fileMute.load()); InvalidateRect(file.mute, nullptr, FALSE); break;
        case OutMute: setOutputMute(!engine.outMute.load()); break;
        case SelectMic: detail = Detail::Mic; updateControls(); break;
        case SelectFile: detail = Detail::File; updateControls(); break;
        case VoiceEq: engine.voiceEq.store(!engine.voiceEq.load()); InvalidateRect(voiceEq, nullptr, FALSE); break;
        case Ducking: engine.ducking.store(!engine.ducking.load()); InvalidateRect(ducking, nullptr, FALSE); break;
        case Loop: engine.loop.store(!engine.loop.load()); InvalidateRect(loop, nullptr, FALSE); break;
        case Play:
            if (active && !path.empty() && engine.running()) {
                bool wasPlaying = engine.playing.load();
                while (!engine.playing.compare_exchange_weak(wasPlaying, !wasPlaying)) {}
                if (!statusError) showOutputStatus(tr(L"运行中。", L"Sending."));
                updatePlay();
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
        app->layout();
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
    case WM_HSCROLL:
        app->gainChanged(reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_TIMER:
        if (wParam == kPollTimer) app->poll();
        return 0;
    case WM_NOTIFY: {
        auto* notification = reinterpret_cast<NMHDR*>(lParam);
        if (notification->code == NM_CUSTOMDRAW &&
            (notification->hwndFrom == app->mic.gain || notification->hwndFrom == app->file.gain))
            return app->drawSlider(*reinterpret_cast<NMCUSTOMDRAW*>(notification));
        if (notification->hwndFrom == app->tooltip && notification->code == TTN_GETDISPINFOW) {
            auto* tip = reinterpret_cast<NMTTDISPINFOW*>(notification);
            tip->lpszText = app->path.empty() ? tip->szText : app->path.data();
            if (app->path.empty()) tip->szText[0] = L'\0';
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
    case WM_DESTROY:
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
