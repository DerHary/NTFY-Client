#pragma once

#include "client.hpp"
#include <memory>

class Delivery {
public:
    enum class Purpose { Receive, Test };

    Delivery(HINSTANCE instance, NotificationPreferences preferences, Purpose purpose = Purpose::Receive);
    ~Delivery();
    Delivery(const Delivery&) = delete;
    Delivery& operator=(const Delivery&) = delete;

    void Publish(Notification notification);
    void Pump(HANDLE stop);
    void ShowTest(HWND owner, unsigned linkCount);

private:
    struct State;
    std::unique_ptr<State> state_;
};
