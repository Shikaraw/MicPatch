// Settings dialog members of App, included from main.cpp.
    bool storePreferences(micpatch::Settings next, HWND owner) {
        const auto error = micpatch::saveSettings(next);
        if (error != ERROR_SUCCESS) {
            const auto message = tr(L"无法保存设置（错误 ", L"Could not save settings (error ") +
                                 std::to_wstring(error) + tr(L"）。", L").");
            MessageBoxW(owner, message.c_str(), tr(L"设置", L"Settings"), MB_OK | MB_ICONWARNING);
            return false;
        }
        const bool hotkeyChanged = next.hotkey != preferences.hotkey;
        preferences = std::move(next);
        if (hotkeyChanged) {
            unregisterMuteHotkey();
            registerMuteHotkey();
        }
        return true;
    }

    bool applyAutoSave(bool enabled, bool rememberChanged, HWND owner) {
        const auto warnSave = [&](const wchar_t* text, LSTATUS error) {
            const auto detail = text + std::to_wstring(error) + tr(L"）。", L").");
            MessageBoxW(owner, detail.c_str(), tr(L"设置", L"Settings"), MB_OK | MB_ICONWARNING);
        };
        const bool changed = enabled != autoSave;
        if (changed) {
            const auto error = micpatch::setAutoSaveEnabled(enabled);
            if (error != ERROR_SUCCESS) {
                warnSave(tr(L"无法更改自动保存设置（错误 ", L"Could not change the autosave setting (error "), error);
                if (rememberChanged && autoSave) { saveDirty = true; flushSession(); }
                return false;
            }
            autoSave = enabled;
        }
        if (!autoSave) {
            KillTimer(window, kSaveTimer);
            saveDirty = false;
            saveError = ERROR_SUCCESS;
            const auto error = micpatch::clearSession();
            if (error != ERROR_SUCCESS) {
                warnSave(tr(L"自动保存已关闭，但无法删除旧会话（错误 ", L"Autosave is off, but the old session could not be deleted (error "), error);
            }
            InvalidateRect(window, &footer, FALSE);
        } else if (changed || rememberChanged || saveError != ERROR_SUCCESS) {
            saveDirty = true;
            if (!flushSession()) {
                warnSave(tr(L"无法保存会话（错误 ", L"Could not save the session (error "), saveError);
                return false;
            }
        }
        return true;
    }

    static INT_PTR CALLBACK settingsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_INITDIALOG) {
            const auto& app = *reinterpret_cast<App*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, lParam);
            SetWindowTextW(dialog, tr(L"设置", L"Settings"));
            SetDlgItemTextW(dialog, IDC_SETTINGS_LANGUAGE_LABEL, tr(L"界面语言", L"Language"));
            SetDlgItemTextW(dialog, IDC_SETTINGS_THEME_LABEL, tr(L"主题", L"Theme"));
            SetDlgItemTextW(dialog, IDC_SETTINGS_HOTKEY_LABEL, tr(L"总静音热键", L"Master mute hotkey"));
            SetDlgItemTextW(dialog, IDC_SETTINGS_REMEMBER,
                tr(L"记住输出设备（不会自动发送）", L"Remember output device (never auto-starts sending)"));
            SetDlgItemTextW(dialog, IDC_SETTINGS_AUTOSAVE,
                tr(L"自动保存音源与混音设置（不会自动播放）", L"Autosave sources and mix settings (no autoplay)"));
            SetDlgItemTextW(dialog, IDC_SETTINGS_START_MUTED,
                tr(L"启动应用时默认总静音", L"Mute all output when the app starts"));
            SetDlgItemTextW(dialog, IDC_SETTINGS_NOTE,
                tr(L"语言、主题和启动静音将在下次打开声搭时生效。热键立即生效，不会中断发送。",
                   L"Language, theme, and startup mute apply the next time you open MicPatch. "
                   L"Hotkey changes apply now without interrupting audio."));
            SetDlgItemTextW(dialog, IDOK, tr(L"保存", L"Save"));
            SetDlgItemTextW(dialog, IDCANCEL, tr(L"取消", L"Cancel"));
            const auto fillChoices = [dialog](int control, int selected, std::initializer_list<const wchar_t*> choices) {
                for (const auto* item : choices)
                    SendDlgItemMessageW(dialog, control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
                SendDlgItemMessageW(dialog, control, CB_SETCURSEL, selected, 0);
            };
            fillChoices(IDC_SETTINGS_LANGUAGE, app.preferences.english, {L"简体中文", L"English"});
            fillChoices(IDC_SETTINGS_THEME, static_cast<int>(app.preferences.theme),
                {tr(L"石墨深色", L"Graphite Dark"), tr(L"海蓝深色", L"Ocean Dark"), tr(L"浅色", L"Light")});
            fillChoices(IDC_SETTINGS_HOTKEY, static_cast<int>(app.preferences.hotkey),
                {muteHotkeyName(micpatch::MuteHotkey::Standard), muteHotkeyName(micpatch::MuteHotkey::Alternate),
                 tr(L"关闭", L"Off")});
            CheckDlgButton(dialog, IDC_SETTINGS_REMEMBER, app.preferences.rememberOutput ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog, IDC_SETTINGS_START_MUTED, app.preferences.startMuted ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog, IDC_SETTINGS_AUTOSAVE, app.autoSave ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL)) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (message == WM_COMMAND && LOWORD(wParam) == IDOK) {
            auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(dialog, DWLP_USER));
            if (!app) return FALSE;
            auto next = app->preferences;
            const auto lang = SendDlgItemMessageW(dialog, IDC_SETTINGS_LANGUAGE, CB_GETCURSEL, 0, 0);
            const auto theme = SendDlgItemMessageW(dialog, IDC_SETTINGS_THEME, CB_GETCURSEL, 0, 0);
            const auto hotkey = SendDlgItemMessageW(dialog, IDC_SETTINGS_HOTKEY, CB_GETCURSEL, 0, 0);
            if (lang < 0 || lang > 1 || theme < 0 || theme > 2 || hotkey < 0 || hotkey > 2) return TRUE;
            next.english = lang == 1;
            next.theme = static_cast<micpatch::Theme>(theme);
            next.hotkey = static_cast<micpatch::MuteHotkey>(hotkey);
            next.rememberOutput = IsDlgButtonChecked(dialog, IDC_SETTINGS_REMEMBER) == BST_CHECKED;
            next.startMuted = IsDlgButtonChecked(dialog, IDC_SETTINGS_START_MUTED) == BST_CHECKED;
            const bool rememberChanged = next.rememberOutput != app->preferences.rememberOutput;
            next.outputId = next.rememberOutput ? app->outputId : std::wstring{};
            const bool enabled = IsDlgButtonChecked(dialog, IDC_SETTINGS_AUTOSAVE) == BST_CHECKED;
            if (app->storePreferences(std::move(next), dialog) && app->applyAutoSave(enabled, rememberChanged, dialog))
                EndDialog(dialog, IDOK);
            return TRUE;
        }
        return FALSE;
    }

    void showSettings() {
        if (DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), window, settingsProc,
                            reinterpret_cast<LPARAM>(this)) == -1) {
            MessageBoxW(window, tr(L"无法打开设置窗口。", L"Could not open the settings window."),
                        tr(L"设置", L"Settings"), MB_OK | MB_ICONWARNING);
        }
    }
