#include "client.hpp"
#include <shellapi.h>
#include <shobjidl.h>
#include <array>
#include <memory>

MutexLock::MutexLock(const wchar_t* name, DWORD timeout)
    : handle_(CreateMutexW(nullptr, FALSE, name))
{
    winrt::check_bool(static_cast<bool>(handle_));
    const DWORD wait = WaitForSingleObject(handle_.get(), timeout);
    if (wait == WAIT_FAILED) winrt::throw_last_error();
    owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
}

MutexLock::~MutexLock()
{
    if (owned_) ReleaseMutex(handle_.get());
}

bool StopReceiver()
{
    winrt::handle receiver(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, ReceiverMutex));
    if (!receiver) return GetLastError() == ERROR_FILE_NOT_FOUND;

    const auto deadline = GetTickCount64() + 6000;
    do {
        winrt::handle stop(OpenEventW(EVENT_MODIFY_STATE, FALSE, ReceiverStop));
        if (stop) SetEvent(stop.get());
        const DWORD wait = WaitForSingleObject(receiver.get(), 50);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            ReleaseMutex(receiver.get());
            return true;
        }
        if (wait == WAIT_FAILED) return false;
    } while (GetTickCount64() < deadline);
    return false;
}

bool StartReceiver()
{
    std::array<wchar_t, 32768> file{};
    const DWORD length = GetModuleFileNameW(nullptr, file.data(), static_cast<DWORD>(file.size()));
    if (!length || length >= file.size()) return false;
    auto command = L"\"" + std::wstring(file.data(), length) + L"\" --receive";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(file.data(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
        nullptr, &startup, &info)) return false;
    winrt::handle process(info.hProcess);
    winrt::handle thread(info.hThread);

    const auto deadline = GetTickCount64() + 5000;
    do {
        winrt::handle ready(OpenEventW(SYNCHRONIZE, FALSE, ReceiverStop));
        if (ready) return true;
        if (WaitForSingleObject(process.get(), 30) != WAIT_TIMEOUT) return false;
    } while (GetTickCount64() < deadline);
    return false;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int count = 0;
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> args(
        CommandLineToArgvW(GetCommandLineW(), &count), LocalFree);
    if (!args) return 1;
    const std::wstring_view mode = count == 2 ? args.get()[1] : L"";
    const bool background = mode == L"--receive" || mode == L"--stop";
    try {
        if (mode == L"--stop") {
            MutexLock control(ControlMutex, 8000);
            return control && StopReceiver() ? 0 : 1;
        }
        winrt::init_apartment(background ? winrt::apartment_type::multi_threaded :
            winrt::apartment_type::single_threaded);
        winrt::check_hresult(SetCurrentProcessExplicitAppUserModelID(AppId));
        if (mode == L"--receive") return RunReceiver();
        if (count == 1 || mode == L"--settings") return OpenSettings(instance);
    } catch (...) {
        if (!background) MessageBoxW(nullptr, L"Die Anwendung konnte nicht gestartet werden.",
            SettingsTitle, MB_OK | MB_ICONERROR);
    }
    return 1;
}
