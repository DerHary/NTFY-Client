#pragma once

#include "client.hpp"
#include <mmsystem.h>

std::vector<std::wstring> ListSounds();
bool OpenSoundFolder(HWND owner);

class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool Play(const std::wstring& name, HWND callback);
    void Finished(WPARAM status, LPARAM device);
    void Stop();
    static void PlayDefault();

private:
    MCIDEVICEID device_ = 0;
};
