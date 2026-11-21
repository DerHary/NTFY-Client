#include "client.hpp"
#include "resource.h"

#include <algorithm>
#include <optional>

namespace {
struct SettingsState {
    HINSTANCE instance;
    std::vector<Subscription> subscriptions;
    bool dirty = false;
    bool receiverPending = false;
};

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
        SetWindowTextW(window, state.index ? L"ntfy – Abo bearbeiten" : L"ntfy – Abo hinzufügen");
        if (state.index) {
            const auto& sub = state.subscriptions.at(*state.index);
            SetDlgItemTextW(window, IDC_SERVER, sub.server.c_str());
            SetDlgItemTextW(window, IDC_TOPIC, sub.topic.c_str());
            try {
                auto token = UnprotectToken(sub.protectedToken);
                SetDlgItemTextW(window, IDC_TOKEN, token.c_str());
                SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
            } catch (...) {
                Error(window, L"Der gespeicherte Token ist nicht lesbar. Bitte erneut eingeben.");
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
            throw UserError{L"Ein Topic braucht 1 bis 64 Zeichen. Erlaubt sind Buchstaben A–Z, Zahlen, Bindestrich und Unterstrich."};
        }
        if (!ValidToken(token)) throw UserError{L"Der Zugriffstoken ist zu lang oder enthält ungültige Zeichen."};
        for (size_t i = 0; i < state.subscriptions.size(); ++i) {
            const auto& existing = state.subscriptions[i];
            if (state.index != i && existing.server == server && existing.topic == topic) {
                throw UserError{L"Dieses Topic ist auf diesem Server bereits abonniert."};
            }
        }
        state.result = {std::move(server), std::move(topic), ProtectToken(token)};
        SetDlgItemTextW(window, IDC_TOKEN, L"");
        EndDialog(window, IDOK);
    } catch (const UserError& issue) {
        Error(window, issue.message);
    } catch (...) {
        Error(window, L"Das Abo konnte nicht übernommen werden.");
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
    EnableWindow(GetDlgItem(window, IDC_ADD), state.subscriptions.size() < MaxSubscriptions);
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
    for (const auto& sub : state.subscriptions) {
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
    SetDlgItemTextW(window, IDC_STATUS, state.dirty ? L"Ungespeicherte Änderungen." :
        state.subscriptions.empty() ? L"Noch keine Abos eingerichtet." : L"");
}

void Edit(HWND window, std::optional<size_t> index)
{
    auto& state = DialogState<SettingsState>(window);
    if (!index && state.subscriptions.size() >= MaxSubscriptions) return;
    EditState editor{state.subscriptions, index, {}};
    const auto result = DialogBoxParamW(state.instance, MAKEINTRESOURCEW(DLG_SUBSCRIPTION),
        window, EditProc, reinterpret_cast<LPARAM>(&editor));
    if (result == -1) Error(window, L"Der Abo-Dialog konnte nicht geöffnet werden.");
    if (result != IDOK) return;
    if (index) {
        state.subscriptions[*index] = std::move(editor.result);
    } else {
        index = state.subscriptions.size();
        state.subscriptions.push_back(std::move(editor.result));
    }
    state.dirty = true;
    Refresh(window, index);
}

void Save(HWND window)
{
    auto& state = DialogState<SettingsState>(window);
    MutexLock control(ControlMutex, 100);
    if (!control) throw UserError{L"Der Empfänger wird gerade geändert. Bitte erneut versuchen."};

    if (state.dirty) {
        SaveSettings(state.subscriptions);
        state.dirty = false;
    }
    state.receiverPending = true;
    UpdateButtons(window);
    if (!StopReceiver() || !StartReceiver()) {
        SetDlgItemTextW(window, IDC_STATUS, L"Gespeichert. Neustart steht noch aus.");
        throw UserError{L"Die Abos wurden gespeichert. Der Empfänger konnte nicht neu gestartet werden. Erneut auf Speichern klicken, um es noch einmal zu versuchen."};
    }
    state.receiverPending = false;
    UpdateButtons(window);
    SetDlgItemTextW(window, IDC_STATUS, L"Gespeichert. Der Empfänger wurde neu gestartet.");
}

INT_PTR CALLBACK SettingsProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lparam);
        Refresh(window, std::nullopt);
        return TRUE;
    }
    if (message == WM_CLOSE) return SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
    if (message != WM_COMMAND) return FALSE;

    auto& state = DialogState<SettingsState>(window);
    try {
        switch (LOWORD(wparam)) {
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
                state.subscriptions.erase(state.subscriptions.begin() + *index);
                state.dirty = true;
                Refresh(window, state.subscriptions.empty() ? std::nullopt :
                    std::optional<size_t>(std::min(*index, state.subscriptions.size() - 1)));
            }
            return TRUE;
        case IDC_SAVE:
            if (state.dirty || state.receiverPending) Save(window);
            return TRUE;
        case IDCANCEL:
            if (!state.dirty || MessageBoxW(window, L"Ungespeicherte Änderungen verwerfen?",
                SettingsTitle, MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES) {
                EndDialog(window, IDCANCEL);
            }
            return TRUE;
        }
    } catch (const UserError& issue) {
        Error(window, issue.message);
        return TRUE;
    } catch (...) {
        Error(window, L"Die Aktion konnte nicht abgeschlossen werden.");
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

    std::vector<Subscription> subscriptions;
    try {
        subscriptions = LoadSettings();
    } catch (const UserError& issue) {
        Error(nullptr, issue.message);
        return 1;
    } catch (...) {
        Error(nullptr, L"Die Einstellungen konnten nicht gelesen werden.");
        return 1;
    }
    SettingsState state{instance, std::move(subscriptions)};
    return DialogBoxParamW(instance, MAKEINTRESOURCEW(DLG_SETTINGS), nullptr, SettingsProc,
        reinterpret_cast<LPARAM>(&state)) == -1 ? 1 : 0;
}
