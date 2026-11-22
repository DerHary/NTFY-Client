#include "audio.hpp"

#include <shlwapi.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
fs::path SoundDirectory()
{
    std::array<wchar_t, 32768> file{};
    const auto length = GetModuleFileNameW(nullptr, file.data(), static_cast<DWORD>(file.size()));
    if (!length || length >= file.size()) return {};
    return fs::path(file.data()).parent_path() / L"sounds";
}

bool IsAudio(const fs::path& path)
{
    const auto extension = path.extension().native();
    return _wcsicmp(extension.c_str(), L".mp3") == 0 || _wcsicmp(extension.c_str(), L".wav") == 0;
}

bool IsLocalFile(const fs::path& path)
{
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
}

std::vector<std::wstring> ListSounds()
{
    std::vector<std::wstring> files;
    const auto directory = SoundDirectory();
    if (directory.empty()) return files;
    std::error_code error;
    const fs::directory_iterator end;
    for (fs::directory_iterator entry(directory, error); !error && entry != end; entry.increment(error)) {
        if (IsAudio(entry->path()) && IsLocalFile(entry->path())) {
            files.push_back(entry->path().filename().native());
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return StrCmpLogicalW(left.c_str(), right.c_str()) < 0;
    });
    return files;
}

bool OpenSoundFolder(HWND owner)
{
    const auto directory = SoundDirectory();
    if (directory.empty()) return false;
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) return false;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", directory.c_str(),
        nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

AudioPlayer::~AudioPlayer()
{
    Stop();
}

void AudioPlayer::Stop()
{
    if (!device_) return;
    mciSendCommandW(device_, MCI_CLOSE, MCI_WAIT, 0);
    device_ = 0;
}

bool AudioPlayer::Play(const std::wstring& name, HWND callback)
{
    Stop();
    if (name.empty() || name.size() > 255 || name.find_first_of(L"/\\:") != std::wstring::npos ||
        name.find(L'\0') != std::wstring::npos || !IsAudio(name)) return false;
    const auto directory = SoundDirectory();
    if (directory.empty()) return false;
    const auto path = directory / name;
    if (!IsLocalFile(path)) return false;

    MCI_OPEN_PARMSW open{};
    open.lpstrDeviceType = _wcsicmp(path.extension().c_str(), L".wav") == 0 ? L"waveaudio" : L"mpegvideo";
    open.lpstrElementName = path.c_str();
    if (mciSendCommandW(0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_ELEMENT | MCI_WAIT,
        reinterpret_cast<DWORD_PTR>(&open))) return false;
    device_ = open.wDeviceID;

    MCI_PLAY_PARMS play{};
    play.dwCallback = reinterpret_cast<DWORD_PTR>(callback);
    if (!mciSendCommandW(device_, MCI_PLAY, MCI_NOTIFY, reinterpret_cast<DWORD_PTR>(&play))) return true;
    Stop();
    return false;
}

void AudioPlayer::Finished(WPARAM status, LPARAM device)
{
    if (static_cast<MCIDEVICEID>(device) == device_ &&
        (status == MCI_NOTIFY_SUCCESSFUL || status == MCI_NOTIFY_FAILURE)) Stop();
}

void AudioPlayer::PlayDefault()
{
    if (!PlaySoundW(L"Notification.Default", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT)) {
        MessageBeep(MB_OK);
    }
}
