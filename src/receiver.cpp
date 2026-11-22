#include "client.hpp"
#include "delivery.hpp"
#include "notification_details.hpp"

#include <winhttp.h>
#include <powrprof.h>
#include <winrt/Windows.Data.Json.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <thread>

using winrt::Windows::Data::Json::JsonObject;

namespace {
constexpr DWORD ConnectTimeout = 15000;
constexpr DWORD ReadTimeout = 90000;
constexpr size_t MaxLine = 256 * 1024;

struct HttpHandle {
    HINTERNET value;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};

class Request {
public:
    explicit Request(HINTERNET handle) : handle_(handle)
    {
        winrt::check_bool(handle_ != nullptr);
        try {
            completed_.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            closed_.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            winrt::check_bool(completed_ && closed_);
            DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
            winrt::check_bool(WinHttpSetOption(handle_, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)));
            callbackRegistered_ = WinHttpSetStatusCallback(handle_, Callback,
                WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) !=
                WINHTTP_INVALID_STATUS_CALLBACK;
            winrt::check_bool(callbackRegistered_);
        } catch (...) {
            Close();
            throw;
        }
    }

    ~Request() { Close(); }
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    HINTERNET Get() const { return handle_; }

    bool Await(BOOL started, HANDLE stop, HANDLE reconnect, DWORD expected, DWORD timeout)
    {
        if (!started && GetLastError() != ERROR_IO_PENDING) return false;
        const HANDLE events[] = {stop, reconnect, completed_.get()};
        return WaitForMultipleObjects(3, events, FALSE, timeout) == WAIT_OBJECT_0 + 2 &&
            status_.load(std::memory_order_acquire) == expected;
    }

    DWORD Transferred() const { return transferred_.load(std::memory_order_relaxed); }

private:
    static void CALLBACK Callback(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD length)
    {
        if (!context) return;
        auto& self = *reinterpret_cast<Request*>(context);
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            SetEvent(self.closed_.get());
            break;
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            self.transferred_.store(status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE ?
                *static_cast<const DWORD*>(info) : length, std::memory_order_relaxed);
            self.status_.store(status, std::memory_order_release);
            SetEvent(self.completed_.get());
            break;
        }
    }

    void Close()
    {
        // WinHTTP may still own the read buffer and call us after CloseHandle returns.
        WinHttpCloseHandle(handle_);
        if (callbackRegistered_) WaitForSingleObject(closed_.get(), INFINITE);
    }

    HINTERNET handle_;
    winrt::handle completed_;
    winrt::handle closed_;
    bool callbackRegistered_ = false;
    std::atomic<DWORD> status_{0};
    std::atomic<DWORD> transferred_{0};
};

struct Cursor {
    std::deque<std::wstring> seen;
    std::wstring since = std::to_wstring(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
};

void HandleLine(const std::string& line, const Subscription& sub,
    Delivery& delivery, Cursor& cursor)
{
    JsonObject event{nullptr};
    if (!JsonObject::TryParse(winrt::to_hstring(line), event) ||
        event.GetNamedString(L"event", L"") != L"message") return;

    const std::wstring id(event.GetNamedString(L"id", L""));
    if (!id.empty() && std::find(cursor.seen.begin(), cursor.seen.end(), id) != cursor.seen.end()) return;

    Notification notification{event.GetNamedString(L"title", L"").c_str(),
        event.GetNamedString(L"message", L"").c_str(), {}};
    if (notification.title.empty()) notification.title = sub.topic;
    notification.details = IncomingDetails(event, sub.topic, notification.text);
    delivery.Publish(std::move(notification));

    if (ValidTopic(id)) {
        cursor.since = id;
        cursor.seen.push_back(id);
        if (cursor.seen.size() > 256) cursor.seen.pop_front();
    }
}

bool Stream(const Subscription& sub, HANDLE stop, HANDLE reconnect,
    Delivery& delivery, Cursor& cursor)
{
    const std::wstring url = sub.server + L"/" + sub.topic + L"/json?since=" + cursor.since;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    winrt::check_bool(WinHttpCrackUrl(url.c_str(), 0, 0, &parts));
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    HttpHandle session{WinHttpOpen(L"NTFY Client/1.0.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
    winrt::check_bool(session.value != nullptr);
    winrt::check_bool(WinHttpSetTimeouts(session.value, ConnectTimeout, ConnectTimeout,
        ConnectTimeout, ReadTimeout));
    HttpHandle connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    winrt::check_bool(connection.value != nullptr);

    // The buffer lives longer than the request, including during asynchronous cancellation.
    char buffer[4096];
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Request request(WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    winrt::check_bool(WinHttpSetOption(request.Get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)));
    winrt::check_bool(WinHttpSetOption(request.Get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon)));

    if (!sub.protectedToken.empty()) {
        auto token = UnprotectToken(sub.protectedToken);
        if (!ValidToken(token) || token.empty() || !flags) {
            SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
            return false;
        }
        auto header = L"Authorization: Bearer " + token;
        SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
        const BOOL added = WinHttpAddRequestHeaders(request.Get(), header.c_str(),
            static_cast<DWORD>(header.size()), WINHTTP_ADDREQ_FLAG_ADD);
        SecureZeroMemory(header.data(), header.size() * sizeof(wchar_t));
        winrt::check_bool(added);
    }

    if (!request.Await(WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, reinterpret_cast<DWORD_PTR>(&request)), stop, reconnect,
        WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, ConnectTimeout)) return false;
    if (!request.Await(WinHttpReceiveResponse(request.Get(), nullptr), stop, reconnect,
        WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, ConnectTimeout)) return false;

    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) return false;

    std::string line;
    bool tooLong = false;
    bool received = false;
    // Request only the bytes currently available so quiet topics deliver short messages immediately.
    while (request.Await(WinHttpQueryDataAvailable(request.Get(), nullptr), stop, reconnect,
        WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, ReadTimeout)) {
        const DWORD available = std::min(request.Transferred(), static_cast<DWORD>(sizeof(buffer)));
        if (!available || !request.Await(WinHttpReadData(request.Get(), buffer, available, nullptr),
            stop, reconnect, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, ReadTimeout)) break;
        const DWORD read = request.Transferred();
        if (!read) break;
        received = true;
        for (DWORD i = 0; i < read; ++i) {
            if (buffer[i] == '\n') {
                if (!tooLong && !line.empty()) {
                    try {
                        HandleLine(line, sub, delivery, cursor);
                    } catch (...) {
                        // Invalid payloads must not break the stream.
                    }
                }
                line.clear();
                tooLong = false;
            } else if (line.size() < MaxLine) {
                line.push_back(buffer[i]);
            } else {
                tooLong = true;
            }
        }
    }
    return received;
}

void Receive(const Subscription& sub, HANDLE stop, HANDLE reconnect, Delivery& delivery)
{
    winrt::init_apartment();
    try {
        Cursor cursor;
        DWORD delay = 1000;
        while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
            ResetEvent(reconnect);
            try {
                if (Stream(sub, stop, reconnect, delivery, cursor)) delay = 1000;
            } catch (...) {
                // Connection and token failures are retried without logging credentials or URLs.
            }
            const HANDLE events[] = {stop, reconnect};
            const auto wait = WaitForMultipleObjects(2, events, FALSE, delay);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
            delay = wait == WAIT_OBJECT_0 + 1 ? 1000 : std::min(delay * 2, 30000UL);
        }
    } catch (...) {
        OutputDebugStringW(L"ntfy: receiver worker could not start.\n");
    }
    winrt::uninit_apartment();
}

ULONG CALLBACK OnPower(PVOID context, ULONG type, PVOID)
{
    if (type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND) {
        for (const auto& event : *static_cast<std::vector<winrt::handle>*>(context)) {
            SetEvent(event.get());
        }
    }
    return ERROR_SUCCESS;
}
}

int RunReceiver(HINSTANCE windowInstance)
{
    MutexLock instance(ReceiverMutex);
    if (!instance) return 0;
    const auto settings = LoadSettings();
    const auto& subscriptions = settings.subscriptions;
    Delivery delivery(windowInstance, settings.notifications);

    winrt::handle stop(CreateEventW(nullptr, TRUE, FALSE, ReceiverStop));
    winrt::check_bool(static_cast<bool>(stop));
    std::vector<winrt::handle> reconnect;
    reconnect.reserve(subscriptions.size());
    for (size_t i = 0; i < subscriptions.size(); ++i) {
        winrt::handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        winrt::check_bool(static_cast<bool>(event));
        reconnect.push_back(std::move(event));
    }

    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS callback{OnPower, &reconnect};
    HPOWERNOTIFY power = nullptr;
    const bool registered = PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK,
        reinterpret_cast<HANDLE>(&callback), &power) == ERROR_SUCCESS;

    std::vector<std::thread> workers;
    int result = 0;
    try {
        workers.reserve(subscriptions.size());
        for (size_t i = 0; i < subscriptions.size(); ++i) {
            workers.emplace_back(Receive, std::cref(subscriptions[i]), stop.get(), reconnect[i].get(),
                std::ref(delivery));
        }
        delivery.Pump(stop.get());
        SetEvent(stop.get());
    } catch (...) {
        SetEvent(stop.get());
        result = 1;
    }
    if (registered) PowerUnregisterSuspendResumeNotification(power);
    for (auto& thread : workers) thread.join();
    return result;
}
