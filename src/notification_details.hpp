#pragma once

#include "client.hpp"
#include <winrt/Windows.Data.Json.h>

using winrt::Windows::Data::Json::JsonObject;

NotificationDetails IncomingDetails(const JsonObject& event, const std::wstring& topic,
    const std::wstring& text);
NotificationDetails ReadSavedDetails(const JsonObject& reminder);
JsonObject WriteDetails(const NotificationDetails& details);
bool IsWebLink(std::wstring_view url);
