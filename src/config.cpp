#include "client.hpp"
#include "notification_details.hpp"

#include <shlobj.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>

namespace fs = std::filesystem;
using namespace winrt::Windows::Data::Json;

namespace {
constexpr auto MaxConfigBytes = 2 * 1024 * 1024;

fs::path ConfigPath(const wchar_t* name)
{
    PWSTR raw = nullptr;
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw));
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(raw, CoTaskMemFree);
    return fs::path(folder.get()) / L"NtfyWindows" / name;
}

JsonObject ReadJson(const fs::path& path)
{
    if (!fs::exists(path)) return nullptr;
    if (fs::file_size(path) > MaxConfigBytes) throw UserError{L"The data file is too large."};
    std::ifstream file(path, std::ios::binary);
    if (!file) throw UserError{L"The data file could not be opened."};
    const std::string bytes{std::istreambuf_iterator<char>(file), {}};
    return JsonObject::Parse(winrt::to_hstring(bytes));
}

void WriteJson(const fs::path& path, const JsonObject& root)
{
    const auto bytes = winrt::to_string(root.Stringify());
    if (bytes.size() > MaxConfigBytes) throw UserError{L"The data file is too large."};
    fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp";
    try {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.write(bytes.data(), bytes.size());
        file.close();
        winrt::check_bool(MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    } catch (...) {
        DeleteFileW(temporary.c_str());
        throw;
    }
}

struct WipeString {
    std::wstring& text;
    ~WipeString() { SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t)); }
};

struct ProtectedBuffer {
    DATA_BLOB data{};
    ~ProtectedBuffer()
    {
        if (data.pbData) {
            SecureZeroMemory(data.pbData, data.cbData);
            LocalFree(data.pbData);
        }
    }
};
}

std::wstring NormalizeServer(std::wstring value, bool authenticated)
{
    constexpr auto Error = L"Enter a valid server URL starting with http:// or https://.";
    const auto first = value.find_first_not_of(L" \t");
    if (first == std::wstring::npos) throw UserError{Error};
    value = value.substr(first, value.find_last_not_of(L" \t") - first + 1);
    if (value.size() > 2048 || value.find_first_of(L"?#\\") != std::wstring::npos ||
        value.find_first_of(L" \t\r\n") != std::wstring::npos) throw UserError{Error};

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(value.c_str(), 0, 0, &parts) || !parts.dwHostNameLength ||
        parts.dwUserNameLength || parts.dwPasswordLength ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) {
        throw UserError{Error};
    }
    if (authenticated && parts.nScheme != INTERNET_SCHEME_HTTPS) {
        throw UserError{L"HTTPS is required for a subscription with an access token."};
    }

    try {
        const winrt::Windows::Foundation::Uri uri(value);
        if (std::wstring_view(uri.Host()).empty() || !uri.UserName().empty() ||
            !uri.Password().empty()) throw UserError{Error};
        value = uri.AbsoluteUri().c_str();
    } catch (const winrt::hresult_error&) {
        throw UserError{Error};
    }
    value.erase(value.find_last_not_of(L'/') + 1);
    return value;
}

bool ValidTopic(std::wstring_view value)
{
    return !value.empty() && value.size() <= 64 &&
        value.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") ==
        std::wstring_view::npos;
}

bool ValidToken(std::wstring_view value)
{
    return value.size() <= 512 &&
        value.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~+/=") ==
        std::wstring_view::npos;
}

std::wstring ProtectToken(std::wstring& token)
{
    WipeString wipe{token};
    if (token.empty()) return {};
    DATA_BLOB input{static_cast<DWORD>(token.size() * sizeof(wchar_t)),
        reinterpret_cast<BYTE*>(token.data())};
    ProtectedBuffer output;
    winrt::check_bool(CryptProtectData(&input, L"ntfy", nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &output.data));

    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD length = 0;
    winrt::check_bool(CryptBinaryToStringW(output.data.pbData, output.data.cbData, flags, nullptr, &length));
    std::wstring encoded(length, L'\0');
    winrt::check_bool(CryptBinaryToStringW(output.data.pbData, output.data.cbData, flags,
        encoded.data(), &length));
    encoded.resize(length);
    return encoded;
}

std::wstring UnprotectToken(std::wstring_view protectedToken)
{
    if (protectedToken.empty()) return {};
    const auto length = static_cast<DWORD>(protectedToken.size());
    DWORD size = 0;
    winrt::check_bool(CryptStringToBinaryW(protectedToken.data(), length,
        CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr));
    std::vector<BYTE> decoded(size);
    winrt::check_bool(CryptStringToBinaryW(protectedToken.data(), length,
        CRYPT_STRING_BASE64, decoded.data(), &size, nullptr, nullptr));
    DATA_BLOB input{size, decoded.data()};
    ProtectedBuffer output;
    winrt::check_bool(CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &output.data));
    if (output.data.cbData % sizeof(wchar_t)) throw UserError{L"Invalid token data."};
    return {reinterpret_cast<const wchar_t*>(output.data.pbData), output.data.cbData / sizeof(wchar_t)};
}

Settings LoadSettings()
{
    Settings settings;
    const auto root = ReadJson(ConfigPath(L"subscriptions.json"));
    if (!root) return settings;
    if (root.GetNamedNumber(L"version") != 1) throw UserError{L"Unknown settings format."};
    const auto entries = root.GetNamedArray(L"subscriptions");
    if (entries.Size() > MaxSubscriptions) throw UserError{L"Too many subscriptions in the settings file."};

    const auto mode = root.GetNamedString(L"notificationMode", L"windows");
    if (mode == L"window") settings.notifications.mode = DisplayMode::Window;
    if (mode == L"both") settings.notifications.mode = DisplayMode::Both;
    settings.notifications.sound = root.GetNamedString(L"notificationSound", L"").c_str();
    const auto theme = root.GetNamedString(L"reminderTheme", ReminderThemes[0].key);
    for (size_t index = 0; index < ARRAYSIZE(ReminderThemes); ++index) {
        if (theme == ReminderThemes[index].key) {
            settings.notifications.theme = static_cast<ReminderTheme>(index);
            break;
        }
    }

    for (const auto& value : entries) {
        const auto object = value.GetObject();
        Subscription sub{object.GetNamedString(L"server").c_str(), object.GetNamedString(L"topic").c_str(),
            object.GetNamedString(L"protectedToken", L"").c_str()};
        if (!ValidTopic(sub.topic) || sub.protectedToken.size() > 8192) {
            throw UserError{L"A subscription contains invalid data."};
        }
        sub.server = NormalizeServer(std::move(sub.server), !sub.protectedToken.empty());
        settings.subscriptions.push_back(std::move(sub));
    }
    return settings;
}

void SaveSettings(const Settings& settings)
{
    JsonArray entries;
    for (const auto& sub : settings.subscriptions) {
        JsonObject entry;
        entry.Insert(L"server", JsonValue::CreateStringValue(sub.server));
        entry.Insert(L"topic", JsonValue::CreateStringValue(sub.topic));
        entry.Insert(L"protectedToken", JsonValue::CreateStringValue(sub.protectedToken));
        entries.Append(entry);
    }
    JsonObject root;
    root.Insert(L"version", JsonValue::CreateNumberValue(1));
    root.Insert(L"subscriptions", entries);
    const auto mode = settings.notifications.mode;
    root.Insert(L"notificationMode", JsonValue::CreateStringValue(
        mode == DisplayMode::Both ? L"both" : mode == DisplayMode::Window ? L"window" : L"windows"));
    root.Insert(L"notificationSound", JsonValue::CreateStringValue(settings.notifications.sound));
    const auto theme = static_cast<size_t>(settings.notifications.theme);
    root.Insert(L"reminderTheme", JsonValue::CreateStringValue(ReminderThemes[theme].key));
    WriteJson(ConfigPath(L"subscriptions.json"), root);
}

std::vector<Reminder> LoadReminders()
{
    std::vector<Reminder> reminders;
    const auto root = ReadJson(ConfigPath(L"reminders.json"));
    if (!root) return reminders;
    if (root.GetNamedNumber(L"version") != 1) throw UserError{L"Unknown reminder format."};
    for (const auto& value : root.GetNamedArray(L"reminders")) {
        const auto entry = value.GetObject();
        const auto due = entry.GetNamedNumber(L"due");
        if (!std::isfinite(due) || due < 0 || due > 253402300799.) {
            throw UserError{L"Invalid reminder time."};
        }
        reminders.push_back({entry.GetNamedString(L"id").c_str(),
            {entry.GetNamedString(L"title").c_str(), entry.GetNamedString(L"text").c_str(),
                ReadSavedDetails(entry)},
            static_cast<std::int64_t>(due)});
    }
    return reminders;
}

void SaveReminders(const std::vector<Reminder>& reminders)
{
    JsonArray entries;
    for (const auto& reminder : reminders) {
        JsonObject entry;
        entry.Insert(L"id", JsonValue::CreateStringValue(reminder.id));
        entry.Insert(L"title", JsonValue::CreateStringValue(reminder.notification.title));
        entry.Insert(L"text", JsonValue::CreateStringValue(reminder.notification.text));
        entry.Insert(L"details", WriteDetails(reminder.notification.details));
        entry.Insert(L"due", JsonValue::CreateNumberValue(static_cast<double>(reminder.due)));
        entries.Append(entry);
    }
    JsonObject root;
    root.Insert(L"version", JsonValue::CreateNumberValue(1));
    root.Insert(L"reminders", entries);
    WriteJson(ConfigPath(L"reminders.json"), root);
}
