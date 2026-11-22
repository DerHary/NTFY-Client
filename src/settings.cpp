#include "client.hpp"
#include "audio.hpp"
#include "delivery.hpp"
#include "resource.h"

#include <algorithm>
#include <optional>

namespace {
struct SettingsState {
    HINSTANCE instance;
    Settings settings;
    std::vector<std::wstring> sounds;
    AudioPlayer preview;
    std::unique_ptr<Delivery> testDelivery;
    bool dirty = false;
    bool receiverPending = false;
};

void AddChoice(HWND window, int control, const wchar_t* label)
{
    SendDlgItemMessageW(window, control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
}

struct EditState {
    const std::vector<Subscription>& subscriptions;
    std::optional<size_t> index;
    Subscription result;
};

template<class T>
T& DialogState(HWND window)
{
    return *reinterpret_cast<T*>(GetWindowLongPtrW(window, DWLP_USER));
}

void Error(HWND owner, const wchar_t* message)
{
    MessageBoxW(owner, message, SettingsTitle, MB_OK | MB_ICONWARNING);
}

std::wstring Field(HWND window, int id)
{
    const HWND edit = GetDlgItem(window, id);
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
    const int length = GetWindowTextW(edit, value.data(), static_cast<int>(value.size()));
    value.resize(length);
    return value;
}

INT_PTR CALLBACK EditProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lparam);
        SendDlgItemMessageW(window, IDC_SERVER, EM_LIMITTEXT, 2049, 0);
        SendDlgItemMessageW(window, IDC_TOPIC, EM_LIMITTEXT, 65, 0);
        SendDlgItemMessageW(window, IDC_TOKEN, EM_LIMITTEXT, 513, 0);
        const auto& state = DialogState<EditState>(window);
        SetWindowTextW(window, state.index ? L"NTFY Client – Edit subscription" : L"NTFY Client – Add subscription");
        if (state.index) {
            const auto& sub = state.subscriptions.at(*state.index);
            SetDlgItemTextW(window, IDC_SERVER, sub.server.c_str());
            SetDlgItemTextW(window, IDC_TOPIC, sub.topic.c_str());
            try {
                auto token = UnprotectToken(sub.protectedToken);
                SetDlgItemTextW(window, IDC_TOKEN, token.c_str());
                SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
            } catch (...) {
                Error(window, L"The saved token could not be read. Enter it again.");
            }
        }
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    if (LOWORD(wparam) == IDCANCEL) {
        SetDlgItemTextW(window, IDC_TOKEN, L"");
        EndDialog(window, IDCANCEL);
        return TRUE;
    }
    if (LOWORD(wparam) != IDOK) return FALSE;

    auto& state = DialogState<EditState>(window);
    auto token = Field(window, IDC_TOKEN);
    try {
        auto server = NormalizeServer(Field(window, IDC_SERVER), !token.empty());
        auto topic = Field(window, IDC_TOPIC);
        if (!ValidTopic(topic)) {
            throw UserError{L"Enter 1 to 64 letters, digits, hyphens or underscores for the topic."};
        }
        if (!ValidToken(token)) throw UserError{L"The access token is too long or contains invalid characters."};
        for (size_t i = 0; i < state.subscriptions.size(); ++i) {
            const auto& existing = state.subscriptions[i];
            if (state.index != i && existing.server == server && existing.topic == topic) {
                throw UserError{L"You already subscribed to this topic on this server."};
            }
        }
        state.result = {std::move(server), std::move(topic), ProtectToken(token)};
        SetDlgItemTextW(window, IDC_TOKEN, L"");
        EndDialog(window, IDOK);
    } catch (const UserError& issue) {
        Error(window, issue.message);
    } catch (...) {
        Error(window, L"Could not apply the subscription.");
    }
    SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
    return TRUE;
}

std::optional<size_t> Selected(HWND window)
{
    const auto selected = SendDlgItemMessageW(window, IDC_SUBSCRIPTIONS, LB_GETCURSEL, 0, 0);
    return selected == LB_ERR ? std::nullopt : std::optional<size_t>(selected);
}

void UpdateButtons(HWND window)
{
    const auto& state = DialogState<SettingsState>(window);
    const bool selected = Selected(window).has_value();
    EnableWindow(GetDlgItem(window, IDC_EDIT), selected);
    EnableWindow(GetDlgItem(window, IDC_REMOVE), selected);
    EnableWindow(GetDlgItem(window, IDC_ADD), state.settings.subscriptions.size() < MaxSubscriptions);
    EnableWindow(GetDlgItem(window, IDC_SAVE), state.dirty || state.receiverPending);
}

void Refresh(HWND window, std::optional<size_t> selected)
{
    const auto& state = DialogState<SettingsState>(window);
    const HWND list = GetDlgItem(window, IDC_SUBSCRIPTIONS);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    HDC dc = GetDC(list);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0));
    const auto previous = dc && font ? SelectObject(dc, font) : nullptr;
    LONG width = 0;
    for (const auto& sub : state.settings.subscriptions) {
        const auto label = sub.topic + L"  ·  " + sub.server;
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SIZE measure{};
        if (dc && GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &measure)) {
            width = std::max(width, measure.cx);
        }
    }
    if (previous) SelectObject(dc, previous);
    if (dc) ReleaseDC(list, dc);
    SendMessageW(list, LB_SETHORIZONTALEXTENT, width + 12, 0);
    if (selected) SendMessageW(list, LB_SETCURSEL, *selected, 0);
    UpdateButtons(window);
    SetDlgItemTextW(window, IDC_STATUS, state.dirty ? L"Unsaved changes." :
        state.settings.subscriptions.empty() ? L"No subscriptions yet." : L"");
}

void PopulateSoundChoices(HWND window)
{
    auto& state = DialogState<SettingsState>(window);
    const auto& preferences = state.settings.notifications;
    SendDlgItemMessageW(window, IDC_SOUND, CB_RESETCONTENT, 0, 0);
    AddChoice(window, IDC_SOUND, L"Windows default");
    for (const auto& sound : state.sounds) AddChoice(window, IDC_SOUND, sound.c_str());
    const auto match = std::find_if(state.sounds.begin(), state.sounds.end(),
        [&](const auto& sound) { return _wcsicmp(sound.c_str(), preferences.sound.c_str()) == 0; });
    size_t selection = 0;
    if (match != state.sounds.end()) {
        selection = static_cast<size_t>(match - state.sounds.begin()) + 1;
    } else if (!preferences.sound.empty()) {
        state.sounds.push_back(preferences.sound);
        AddChoice(window, IDC_SOUND, (preferences.sound + L" (missing)").c_str());
        selection = state.sounds.size();
    }
    SendDlgItemMessageW(window, IDC_SOUND, CB_SETCURSEL, selection, 0);
    SendDlgItemMessageW(window, IDC_SOUND, CB_SETDROPPEDWIDTH, MulDiv(285, GetDpiForWindow(window), 96), 0);
}

void InitPreferences(HWND window)
{
    const auto& preferences = DialogState<SettingsState>(window).settings.notifications;
    for (const wchar_t* label : {L"Windows notification only", L"Reminder window only", L"Both"}) {
        AddChoice(window, IDC_DISPLAY_MODE, label);
    }
    SendDlgItemMessageW(window, IDC_DISPLAY_MODE, CB_SETCURSEL, static_cast<WPARAM>(preferences.mode), 0);
    for (const wchar_t* label : {L"White & Blue", L"Midnight Mint", L"Slate & Amber"}) {
        AddChoice(window, IDC_REMINDER_THEME, label);
    }
    SendDlgItemMessageW(window, IDC_REMINDER_THEME, CB_SETCURSEL, static_cast<WPARAM>(preferences.theme), 0);
    for (const wchar_t* label : {L"No link", L"One link", L"Multiple links"}) {
        AddChoice(window, IDC_TEST_LINK_COUNT, label);
    }
    SendDlgItemMessageW(window, IDC_TEST_LINK_COUNT, CB_SETCURSEL, 1, 0);
    PopulateSoundChoices(window);
}

void UpdatePreferences(HWND window)
{
    auto& state = DialogState<SettingsState>(window);
    const auto mode = SendDlgItemMessageW(window, IDC_DISPLAY_MODE, CB_GETCURSEL, 0, 0);
    const auto sound = SendDlgItemMessageW(window, IDC_SOUND, CB_GETCURSEL, 0, 0);
    const auto theme = SendDlgItemMessageW(window, IDC_REMINDER_THEME, CB_GETCURSEL, 0, 0);
    if (mode < 0 || mode > static_cast<LRESULT>(DisplayMode::Both) ||
        theme < 0 || theme > static_cast<LRESULT>(ReminderTheme::SlateAmber) ||
        sound < 0 || static_cast<size_t>(sound) > state.sounds.size()) return;
    auto& preferences = state.settings.notifications;
    preferences.mode = static_cast<DisplayMode>(mode);
    preferences.sound = sound ? state.sounds[static_cast<size_t>(sound) - 1] : L"";
    preferences.theme = static_cast<ReminderTheme>(theme);
    state.dirty = true;
    UpdateButtons(window);
    SetDlgItemTextW(window, IDC_STATUS, L"Unsaved changes.");
}

void Edit(HWND window, std::optional<size_t> index)
{
    auto& state = DialogState<SettingsState>(window);
    auto& subscriptions = state.settings.subscriptions;
    if (!index && subscriptions.size() >= MaxSubscriptions) return;
    EditState editor{subscriptions, index, {}};
    const auto result = DialogBoxParamW(state.instance, MAKEINTRESOURCEW(DLG_SUBSCRIPTION),
        window, EditProc, reinterpret_cast<LPARAM>(&editor));
    if (result == -1) Error(window, L"Could not open the subscription dialog.");
    if (result != IDOK) return;
    if (index) {
        subscriptions[*index] = std::move(editor.result);
    } else {
        index = subscriptions.size();
        subscriptions.push_back(std::move(editor.result));
    }
    state.dirty = true;
    Refresh(window, index);
}

void Save(HWND window)
{
    auto& state = DialogState<SettingsState>(window);
    MutexLock control(ControlMutex, 100);
    if (!control) throw UserError{L"The receiver is busy. Try again."};

    if (state.dirty) {
        SaveSettings(state.settings);
        state.dirty = false;
    }
    state.receiverPending = true;
    UpdateButtons(window);
    if (!StopReceiver() || !StartReceiver()) {
        SetDlgItemTextW(window, IDC_STATUS, L"Saved. Receiver restart pending.");
        throw UserError{L"Settings were saved. The receiver could not restart. Click Save to try again."};
    }
    state.receiverPending = false;
    UpdateButtons(window);
    SetDlgItemTextW(window, IDC_STATUS, L"Saved. The receiver restarted.");
}

INT_PTR CALLBACK SettingsProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lparam);
        InitPreferences(window);
        Refresh(window, std::nullopt);
        return TRUE;
    }
    if (message == WM_CLOSE) return SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
    if (message == MM_MCINOTIFY) {
        DialogState<SettingsState>(window).preview.Finished(wparam, lparam);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;

    auto& state = DialogState<SettingsState>(window);
    auto& subscriptions = state.settings.subscriptions;
    try {
        switch (LOWORD(wparam)) {
        case IDC_SOUND:
            if (HIWORD(wparam) == CBN_DROPDOWN) {
                state.sounds = ListSounds();
                PopulateSoundChoices(window);
            } else if (HIWORD(wparam) == CBN_SELCHANGE) {
                UpdatePreferences(window);
            }
            return TRUE;
        case IDC_DISPLAY_MODE:
        case IDC_REMINDER_THEME:
            if (HIWORD(wparam) == CBN_SELCHANGE) UpdatePreferences(window);
            return TRUE;
        case IDC_PREVIEW:
            state.testDelivery.reset();
            PlaySoundW(nullptr, nullptr, 0);
            if (state.settings.notifications.sound.empty()) {
                state.preview.Stop();
                AudioPlayer::PlayDefault();
            } else if (!state.preview.Play(state.settings.notifications.sound, window)) {
                Error(window, L"The selected sound could not be played.");
            }
            return TRUE;
        case IDC_OPEN_SOUNDS_FOLDER:
            if (!OpenSoundFolder(window)) Error(window, L"Could not open the sounds folder.");
            return TRUE;
        case IDC_TEST_NOTIFICATION:
            state.testDelivery.reset();
            state.preview.Stop();
            PlaySoundW(nullptr, nullptr, 0);
            state.testDelivery = std::make_unique<Delivery>(state.instance, state.settings.notifications,
                Delivery::Purpose::Test);
            state.testDelivery->ShowTest(window, static_cast<unsigned>(std::clamp(
                SendDlgItemMessageW(window, IDC_TEST_LINK_COUNT, CB_GETCURSEL, 0, 0), LRESULT{0}, LRESULT{2})));
            SetDlgItemTextW(window, IDC_STATUS, state.dirty ?
                L"Test complete. Unsaved changes." : L"Test complete.");
            return TRUE;
        case IDC_SUBSCRIPTIONS:
            if (HIWORD(wparam) == LBN_DBLCLK && Selected(window)) Edit(window, Selected(window));
            UpdateButtons(window);
            return TRUE;
        case IDC_ADD:
            Edit(window, std::nullopt);
            return TRUE;
        case IDC_EDIT:
            if (const auto index = Selected(window)) Edit(window, index);
            return TRUE;
        case IDC_REMOVE:
            if (const auto index = Selected(window)) {
                subscriptions.erase(subscriptions.begin() + *index);
                state.dirty = true;
                Refresh(window, subscriptions.empty() ? std::nullopt :
                    std::optional<size_t>(std::min(*index, subscriptions.size() - 1)));
            }
            return TRUE;
        case IDC_SAVE:
            if (state.dirty || state.receiverPending) Save(window);
            return TRUE;
        case IDCANCEL:
            if (!state.dirty || MessageBoxW(window, L"Discard unsaved changes?",
                SettingsTitle, MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES) {
                EndDialog(window, IDCANCEL);
            }
            return TRUE;
        }
    } catch (const UserError& issue) {
        Error(window, issue.message);
        return TRUE;
    } catch (...) {
        Error(window, L"Could not complete the action.");
        return TRUE;
    }
    return FALSE;
}
}

int OpenSettings(HINSTANCE instance)
{
    MutexLock lock(SettingsMutex);
    if (!lock) {
        if (const HWND existing = FindWindowW(L"#32770", SettingsTitle)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    SettingsState state{instance, {}, {}, {}, {}};
    try {
        state.settings = LoadSettings();
        state.sounds = ListSounds();
    } catch (const UserError& issue) {
        Error(nullptr, issue.message);
        return 1;
    } catch (...) {
        Error(nullptr, L"Could not read settings.");
        return 1;
    }
    return DialogBoxParamW(instance, MAKEINTRESOURCEW(DLG_SETTINGS), nullptr, SettingsProc,
        reinterpret_cast<LPARAM>(&state)) == -1 ? 1 : 0;
}
