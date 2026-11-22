#include "delivery.hpp"
#include "audio.hpp"
#include "notification_details.hpp"
#include "reminder_style.hpp"
#include "resource.h"

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <richedit.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>

using winrt::Windows::Data::Xml::Dom::XmlDocument;
using winrt::Windows::UI::Notifications::ToastNotification;
using winrt::Windows::UI::Notifications::ToastNotificationManager;

namespace {
constexpr wchar_t AudioWindowClass[] = L"NtfyWindows.Client.Audio";
constexpr int SecondsPerUnit[] = {60, 3600, 86400};

std::int64_t Now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring NewId()
{
    GUID guid{};
    winrt::check_hresult(CoCreateGuid(&guid));
    wchar_t buffer[40]{};
    StringFromGUID2(guid, buffer, ARRAYSIZE(buffer));
    return buffer;
}

void ShowToast(const Notification& notification, bool silent)
{
    XmlDocument xml;
    xml.LoadXml(L"<toast activationType=\"protocol\" launch=\"ntfy-client:settings\">"
                L"<visual><binding template=\"ToastGeneric\"><text/><text/></binding></visual></toast>");
    const auto fields = xml.GetElementsByTagName(L"text");
    fields.Item(0).AppendChild(xml.CreateTextNode(notification.title));
    fields.Item(1).AppendChild(xml.CreateTextNode(notification.text));
    if (silent) {
        const auto audio = xml.CreateElement(L"audio");
        audio.SetAttribute(L"silent", L"true");
        xml.DocumentElement().AppendChild(audio);
    }
    ToastNotificationManager::CreateToastNotifier(AppId).Show(ToastNotification(xml));
}

void PositionReminder(HWND window, bool show = true)
{
    const UINT flags = SWP_NOSIZE | SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    const auto display = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(display, &monitor)) {
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | flags);
        return;
    }

    RECT size{};
    GetWindowRect(window, &size);
    const auto& area = monitor.rcWork;
    const auto gap = MulDiv(16, GetDpiForWindow(window), 96);
    SetWindowPos(window, HWND_TOPMOST,
        std::max(area.left, area.right - (size.right - size.left) - gap),
        std::max(area.top, area.bottom - (size.bottom - size.top) - gap),
        0, 0, flags);
}

void EnsureRichEdit()
{
    static const HMODULE module = LoadLibraryExW(L"msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    winrt::check_bool(module != nullptr);
}

}

struct Delivery::State {
    struct IncomingNotification {
        Notification notification;
        std::int64_t received;
    };

    struct Card {
        Notification notification;
        std::wstring reminderId;
    };

    State(HINSTANCE instance, NotificationPreferences preferences, Purpose purpose)
        : instance(instance), preferences(std::move(preferences)),
          incomingEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr)), testing(purpose == Purpose::Test)
    {
        winrt::check_bool(static_cast<bool>(incomingEvent));
        try {
            if (!testing) reminders = LoadReminders();
            std::stable_sort(reminders.begin(), reminders.end(),
                [](const Reminder& left, const Reminder& right) { return left.due < right.due; });
        } catch (...) {
            reminderStorageAvailable = false;
        }
        WNDCLASSW window{};
        window.lpfnWndProc = AudioProc;
        window.hInstance = instance;
        window.lpszClassName = AudioWindowClass;
        if (!RegisterClassW(&window)) winrt::check_bool(GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
        audioWindow = CreateWindowExW(0, AudioWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
            nullptr, instance, this);
        winrt::check_bool(audioWindow != nullptr);
    }

    ~State()
    {
        if (popup) DestroyWindow(popup);
        audio.Stop();
        DestroyWindow(audioWindow);
    }

    void Notify(const Notification& message)
    {
        const auto mode = preferences.mode;
        const bool customSound = !preferences.sound.empty() && audio.Play(preferences.sound, audioWindow);
        if (mode == DisplayMode::Window) {
            if (!customSound) AudioPlayer::PlayDefault();
            return;
        }
        try {
            ShowToast(message, customSound);
        } catch (...) {
            if (mode == DisplayMode::Both && !customSound) AudioPlayer::PlayDefault();
        }
    }

    void Enqueue(const Notification& message, std::wstring id = {})
    {
        if (preferences.mode != DisplayMode::Windows) cards.push_back({message, std::move(id)});
        Notify(message);
    }

    void ReceiveIncoming()
    {
        if (quiet && !reminderStorageAvailable) return;
        std::deque<IncomingNotification> batch;
        {
            std::scoped_lock lock(incomingMutex);
            batch.swap(incoming);
        }
        if (batch.empty()) return;

        const auto now = Now();
        const bool dueWaiting = std::any_of(reminders.begin(), reminders.end(), [&](const auto& reminder) {
            return reminder.due <= now && !reminder.presented;
        });
        if (!quiet && !dueWaiting) {
            for (const auto& item : batch) Enqueue(item.notification);
            return;
        }
        for (auto& item : batch) {
            const auto position = std::upper_bound(reminders.begin(), reminders.end(), item.received,
                [](std::int64_t time, const Reminder& entry) { return time < entry.due; });
            reminders.insert(position, Reminder{NewId(), std::move(item.notification), item.received});
        }
        remindersDirty = true;
    }

    void CheckQuiet()
    {
        const bool current = WindowsDoNotDisturb().value_or(false);
        if (current && !quiet) {
            audio.Stop();
            PlaySoundW(nullptr, nullptr, 0);
        }
        quiet = current;
    }

    void CheckDue()
    {
        if (quiet || !cards.empty()) return;
        const auto now = Now();
        const auto due = std::find_if(reminders.begin(), reminders.end(), [&](const Reminder& reminder) {
            return reminder.due <= now && !reminder.presented;
        });
        if (due == reminders.end()) return;

        if (preferences.mode == DisplayMode::Windows) {
            const auto tick = GetTickCount64();
            if (tick < nextBanner) return;
            Notify(due->notification);
            nextBanner = tick + 1500;
            reminders.erase(due);
            remindersDirty = true;
        } else {
            Enqueue(due->notification, due->id);
            due->presented = true;
        }
    }

    void FlushReminders()
    {
        if (!remindersDirty) return;
        try {
            SaveReminders(reminders);
            remindersDirty = false;
        } catch (...) {
            // Keep the current queue in memory and retry without repeating delivery.
        }
    }

    void CheckPopup()
    {
        if (quiet || cards.empty()) {
            if (popup) ShowWindow(popup, SW_HIDE);
            return;
        }
        if (!popup) {
            EnsureRichEdit();
            popup = CreateDialogParamW(instance, MAKEINTRESOURCEW(DLG_REMINDER), nullptr, PopupProc,
                reinterpret_cast<LPARAM>(this));
            winrt::check_bool(popup != nullptr);
        }

        if (!IsWindowVisible(popup)) {
            FillPopup(popup);
            PositionReminder(popup);
        }
        const auto backlog = cards.size() > 1
            ? std::to_wstring(cards.size() - 1) + L" more" : std::wstring{};
        SetDlgItemTextW(popup, IDC_REMINDER_QUEUE, backlog.c_str());
    }

    void FillPopup(HWND window)
    {
        const auto& item = cards.front().notification;
        SetDlgItemTextW(window, IDC_REMINDER_TITLE, item.title.c_str());
        linkRanges = SetReminderBody(window, item);
        ShowWindow(GetDlgItem(window, IDC_REMINDER_OPEN_LINK),
            item.details.links.size() == 1 ? SW_SHOWNA : SW_HIDE);
        SetDlgItemTextW(window, IDC_REMINDER_ERROR, L"");
        SendDlgItemMessageW(window, IDC_REMINDER_UNIT, CB_SETCURSEL, 0, 0);
        SendDlgItemMessageW(window, IDC_REMINDER_AMOUNT, CB_SETCURSEL, 14, 0);
    }

    void OpenLink(HWND window, size_t index) const
    {
        if (cards.empty()) return;
        const auto& links = cards.front().notification.details.links;
        if (index >= links.size() || !IsWebLink(links[index].url)) return;
        if (testing) {
            SetDlgItemTextW(window, IDC_REMINDER_ERROR, L"Test link selected.");
            return;
        }
        const auto result = ShellExecuteW(window, L"open", links[index].url.c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            SetDlgItemTextW(window, IDC_REMINDER_ERROR, L"Could not open the link.");
        }
    }

    void Dismiss(bool snooze)
    {
        if (cards.empty()) return;
        const auto& card = cards.front();
        if (snooze || !card.reminderId.empty()) {
            if (!reminderStorageAvailable) throw UserError{L"The reminder file could not be read."};
            auto pending = reminders;
            if (!card.reminderId.empty()) {
                pending.erase(std::remove_if(pending.begin(), pending.end(),
                    [&](const Reminder& item) { return item.id == card.reminderId; }), pending.end());
            }
            if (snooze) {
                const auto unit = SendDlgItemMessageW(popup, IDC_REMINDER_UNIT, CB_GETCURSEL, 0, 0);
                const auto amount = SendDlgItemMessageW(popup, IDC_REMINDER_AMOUNT, CB_GETCURSEL, 0, 0);
                if (unit < 0 || unit >= ARRAYSIZE(SecondsPerUnit) || amount < 0 || amount >= 60) {
                    throw UserError{L"Choose a reminder interval."};
                }
                const auto due = Now() + (amount + 1) * SecondsPerUnit[unit];
                const auto insertAt = std::upper_bound(pending.begin(), pending.end(), due,
                    [](std::int64_t time, const Reminder& entry) { return time < entry.due; });
                pending.insert(insertAt, Reminder{NewId(), card.notification, due});
            }
            SaveReminders(pending);
            reminders = std::move(pending);
            remindersDirty = false;
        }
        cards.pop_front();
        ShowWindow(popup, SW_HIDE);
        CheckPopup();
    }

    static LRESULT CALLBACK AudioProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE) {
            const auto* request = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(request->lpCreateParams));
        }
        if (message == MM_MCINOTIFY) {
            auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (state) state->audio.Finished(wparam, lparam);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static INT_PTR CALLBACK PopupProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_INITDIALOG) {
            SetWindowLongPtrW(window, DWLP_USER, lparam);
            for (const wchar_t* unit : {L"Minutes", L"Hours", L"Days"}) {
                SendDlgItemMessageW(window, IDC_REMINDER_UNIT, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(unit));
            }
            for (int value = 1; value <= 60; ++value) {
                const auto number = std::to_wstring(value);
                SendDlgItemMessageW(window, IDC_REMINDER_AMOUNT, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(number.c_str()));
            }
            auto& state = *reinterpret_cast<State*>(lparam);
            const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
            LOGFONTW titleFont{};
            if (GetObjectW(font, sizeof(titleFont), &titleFont)) {
                titleFont.lfWeight = FW_SEMIBOLD;
                titleFont.lfHeight = MulDiv(titleFont.lfHeight, 14, 9);
                state.titleFont = CreateFontIndirectW(&titleFont);
                if (state.titleFont) SendDlgItemMessageW(window, IDC_REMINDER_TITLE, WM_SETFONT,
                    reinterpret_cast<WPARAM>(state.titleFont), FALSE);
            }
            StyleReminderWindow(window, state.preferences.theme);
            if (state.testing) {
                state.FillPopup(window);
                PositionReminder(window, false);
            }
            return TRUE;
        }
        if (const auto painted = PaintReminderWindow(window, message, wparam, lparam)) return painted;
        auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, DWLP_USER));
        if (message == WM_DESTROY && state && state->titleFont) {
            DeleteObject(state->titleFont);
            state->titleFont = nullptr;
        }
        const auto command = LOWORD(wparam);
        if (state && message == WM_COMMAND && command == IDC_REMINDER_OPEN_LINK) {
            if (!state->cards.empty() && state->cards.front().notification.details.links.size() == 1) {
                state->OpenLink(window, 0);
            }
            return TRUE;
        }
        if (state && message == WM_NOTIFY && wparam == IDC_REMINDER_TEXT) {
            const auto* notification = reinterpret_cast<const NMHDR*>(lparam);
            if (notification->code == EN_LINK) {
                const auto* link = reinterpret_cast<const ENLINK*>(lparam);
                if (link->msg == WM_LBUTTONUP || (link->msg == WM_KEYDOWN && link->wParam == VK_RETURN)) {
                    const auto range = std::find_if(state->linkRanges.begin(), state->linkRanges.end(),
                        [&](const auto& field) { return link->chrg.cpMin == field.first; });
                    if (range != state->linkRanges.end()) state->OpenLink(window, range->index);
                    return TRUE;
                }
            }
        }
        if (message == WM_COMMAND && HIWORD(wparam) == CBN_SELCHANGE &&
            (command == IDC_REMINDER_UNIT || command == IDC_REMINDER_AMOUNT)) {
            InvalidateRect(reinterpret_cast<HWND>(lparam), nullptr, FALSE);
            return TRUE;
        }
        const bool snooze = message == WM_COMMAND && command == IDC_REMINDER_AGAIN;
        const bool close = message == WM_CLOSE || (message == WM_COMMAND && command == IDCANCEL);
        if ((!snooze && !close) || !state) return FALSE;
        if (state->testing) {
            if (close) EndDialog(window, IDCANCEL);
            else SetDlgItemTextW(window, IDC_REMINDER_ERROR, L"Test only. No reminder was scheduled.");
            return TRUE;
        }
        try {
            state->Dismiss(snooze);
        } catch (const UserError& error) {
            SetDlgItemTextW(window, IDC_REMINDER_ERROR, error.message);
        } catch (...) {
            SetDlgItemTextW(window, IDC_REMINDER_ERROR, L"Could not save the reminder. Try again.");
        }
        return TRUE;
    }

    HINSTANCE instance;
    NotificationPreferences preferences;
    winrt::handle incomingEvent;
    const bool testing;
    std::mutex incomingMutex;
    std::deque<IncomingNotification> incoming;
    std::deque<Card> cards;
    std::vector<Reminder> reminders;
    std::vector<ReminderLinkRange> linkRanges;
    bool reminderStorageAvailable = true;
    bool remindersDirty = false;
    bool quiet = false;
    ULONGLONG nextBanner = 0;
    AudioPlayer audio;
    HWND audioWindow = nullptr;
    HWND popup = nullptr;
    HFONT titleFont = nullptr;
};

Delivery::Delivery(HINSTANCE instance, NotificationPreferences preferences, Purpose purpose)
    : state_(std::make_unique<State>(instance, std::move(preferences), purpose)) {}

Delivery::~Delivery() = default;

void Delivery::ShowTest(HWND owner, unsigned linkCount)
{
    Notification sample{L"Test notification",
        L"Review the November invoice and send it to accounting.", {}};
    sample.details.priority = 4;
    sample.details.time = Now();
    sample.details.topic = L"work";
    sample.details.tags = {L"invoice", L"reminder"};
    if (linkCount >= 1) sample.details.links.push_back({L"Open invoice", L"https://example.com/invoice"});
    if (linkCount >= 2) {
        sample.details.attachment = L"payment-guide.pdf";
        sample.details.links.push_back({L"Open attachment", L"https://example.com/payment-guide.pdf"});
    }
    state_->Notify(sample);
    if (state_->preferences.mode == DisplayMode::Windows) return;
    EnsureRichEdit();
    state_->cards.push_back({sample, {}});
    const auto result = DialogBoxParamW(state_->instance, MAKEINTRESOURCEW(DLG_REMINDER), owner,
        State::PopupProc, reinterpret_cast<LPARAM>(state_.get()));
    state_->cards.pop_front();
    if (result == -1) throw UserError{L"Could not open the test reminder window."};
}

void Delivery::Publish(Notification message)
{
    {
        std::scoped_lock lock(state_->incomingMutex);
        state_->incoming.push_back({std::move(message), Now()});
    }
    SetEvent(state_->incomingEvent.get());
}

void Delivery::Pump(HANDLE stop)
{
    const HANDLE signals[] = {stop, state_->incomingEvent.get()};
    while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
        state_->CheckQuiet();
        state_->ReceiveIncoming();
        state_->FlushReminders();
        state_->CheckDue();
        state_->FlushReminders();
        state_->CheckPopup();

        const DWORD wait = MsgWaitForMultipleObjects(ARRAYSIZE(signals), signals, FALSE,
            state_->quiet || state_->remindersDirty || !state_->reminders.empty() || !state_->cards.empty()
                ? 1000 : INFINITE, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) return;
        if (wait == WAIT_FAILED) winrt::throw_last_error();
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return;
            if (!state_->popup || !IsWindowVisible(state_->popup) || !IsDialogMessageW(state_->popup, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
}
