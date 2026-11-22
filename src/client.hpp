#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

inline constexpr wchar_t AppId[] = L"NtfyWindows.Client";
inline constexpr wchar_t ReceiverMutex[] = L"Local\\NtfyWindows.Client.Receiver";
inline constexpr wchar_t ReceiverStop[] = L"Local\\NtfyWindows.Client.Stop";
inline constexpr wchar_t SettingsMutex[] = L"Local\\NtfyWindows.Client.Settings";
inline constexpr wchar_t ControlMutex[] = L"Local\\NtfyWindows.Client.Control";
inline constexpr wchar_t SettingsTitle[] = L"NTFY Client – Settings";
inline constexpr size_t MaxSubscriptions = 64;

struct Subscription {
    std::wstring server;
    std::wstring topic;
    std::wstring protectedToken;
};

enum class DisplayMode { Windows, Window, Both };
enum class ReminderTheme {
    WhiteBlue, MidnightMint, SlateAmber, RoseQuartz, SeaGlass, LavenderInk, DeepOcean, GraphiteLime, FollowWindows
};

inline constexpr struct {
    const wchar_t* key;
    const wchar_t* label;
} ReminderThemes[] = {
    {L"white-blue", L"White & Blue"},
    {L"midnight-mint", L"Midnight Mint"},
    {L"slate-amber", L"Slate & Amber"},
    {L"rose-quartz", L"Rose Quartz"},
    {L"sea-glass", L"Sea Glass"},
    {L"lavender-ink", L"Lavender Ink"},
    {L"deep-ocean", L"Deep Ocean"},
    {L"graphite-lime", L"Graphite & Lime"},
    {L"follow-windows", L"Follow Windows"},
};

struct NotificationPreferences {
    DisplayMode mode = DisplayMode::Windows;
    std::wstring sound;
    ReminderTheme theme = ReminderTheme::WhiteBlue;
};

struct Settings {
    std::vector<Subscription> subscriptions;
    NotificationPreferences notifications;
};

struct NotificationLink {
    std::wstring label;
    std::wstring url;
};

struct NotificationDetails {
    int priority = 0;
    std::int64_t time = 0;
    std::wstring topic;
    std::vector<std::wstring> tags;
    std::wstring attachment;
    std::vector<NotificationLink> links;
};

struct Notification {
    std::wstring title;
    std::wstring text;
    NotificationDetails details;
};

struct Reminder {
    std::wstring id;
    Notification notification;
    std::int64_t due;
    bool presented = false;
};

struct UserError {
    const wchar_t* message;
};

class MutexLock {
public:
    explicit MutexLock(const wchar_t* name, DWORD timeout = 0);
    ~MutexLock();
    MutexLock(const MutexLock&) = delete;
    MutexLock& operator=(const MutexLock&) = delete;
    explicit operator bool() const { return owned_; }

private:
    winrt::handle handle_;
    bool owned_ = false;
};

std::wstring NormalizeServer(std::wstring value, bool authenticated);
bool ValidTopic(std::wstring_view value);
bool ValidToken(std::wstring_view value);
std::wstring ProtectToken(std::wstring& token);
std::wstring UnprotectToken(std::wstring_view protectedToken);
Settings LoadSettings();
void SaveSettings(const Settings& settings);
std::vector<Reminder> LoadReminders();
void SaveReminders(const std::vector<Reminder>& reminders);
std::optional<bool> WindowsDoNotDisturb();

bool StopReceiver();
bool StartReceiver();
int RunReceiver(HINSTANCE instance);
int OpenSettings(HINSTANCE instance);
