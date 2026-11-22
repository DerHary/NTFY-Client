#pragma once

#include "client.hpp"

void StyleReminderWindow(HWND window, ReminderTheme theme);
void RoundReminderWindow(HWND window);
struct ReminderLinkRange {
    LONG first;
    LONG end;
    size_t index;
};

std::vector<ReminderLinkRange> SetReminderBody(HWND window, const Notification& notification);
INT_PTR PaintReminderWindow(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
