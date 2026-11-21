#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <string>
#include <vector>

inline constexpr wchar_t AppId[] = L"NtfyWindows.Client";
inline constexpr wchar_t ReceiverMutex[] = L"Local\\NtfyWindows.Client.Receiver";
inline constexpr wchar_t ReceiverStop[] = L"Local\\NtfyWindows.Client.Stop";
inline constexpr wchar_t SettingsMutex[] = L"Local\\NtfyWindows.Client.Settings";
inline constexpr wchar_t ControlMutex[] = L"Local\\NtfyWindows.Client.Control";
inline constexpr wchar_t SettingsTitle[] = L"ntfy – Einstellungen";
inline constexpr size_t MaxSubscriptions = 64;

struct Subscription {
    std::wstring server;
    std::wstring topic;
    std::wstring protectedToken;
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
std::vector<Subscription> LoadSettings();
void SaveSettings(const std::vector<Subscription>& subscriptions);

bool StopReceiver();
bool StartReceiver();
int RunReceiver();
int OpenSettings(HINSTANCE instance);
