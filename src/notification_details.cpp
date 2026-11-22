#include "notification_details.hpp"

#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <cmath>

using namespace winrt::Windows::Data::Json;

namespace {
IJsonValue Read(const JsonObject& object, const wchar_t* name, JsonValueType type)
{
    if (!object || !object.HasKey(name)) return nullptr;
    const auto value = object.Lookup(name);
    return value.ValueType() == type ? value : nullptr;
}

std::wstring String(const JsonObject& object, const wchar_t* name)
{
    const auto value = Read(object, name, JsonValueType::String);
    return value ? std::wstring(value.GetString()) : std::wstring{};
}

std::int64_t Number(const JsonObject& object, const wchar_t* name, std::int64_t maximum)
{
    const auto value = Read(object, name, JsonValueType::Number);
    if (!value) return 0;
    const double number = value.GetNumber();
    if (!std::isfinite(number) || number < 1 || number > static_cast<double>(maximum)) return 0;
    return static_cast<std::int64_t>(number);
}

void AddLink(NotificationDetails& details, std::wstring url, std::wstring label)
{
    if (!IsWebLink(url)) return;
    const auto existing = std::find_if(details.links.begin(), details.links.end(),
        [&](const NotificationLink& link) { return link.url == url; });
    if (existing != details.links.end()) return;
    details.links.push_back({std::move(label), std::move(url)});
}

void AddMessageLinks(NotificationDetails& details, const std::wstring& message)
{
    constexpr std::wstring_view delimiters = L" \t\r\n<>\"";
    for (size_t start = 0; start < message.size();) {
        if (_wcsnicmp(message.c_str() + start, L"https://", 8) &&
            _wcsnicmp(message.c_str() + start, L"http://", 7)) {
            ++start;
            continue;
        }

        auto end = message.find_first_of(delimiters, start);
        if (end == std::wstring::npos) end = message.size();
        auto linkEnd = end;
        while (linkEnd > start && std::wstring_view(L".,;!?)]}'").find(message[linkEnd - 1]) !=
            std::wstring_view::npos) {
            const auto candidate = std::wstring_view(message).substr(start, linkEnd - start);
            const auto last = candidate.back();
            const wchar_t opening = last == L')' ? L'(' : last == L']' ? L'[' : last == L'}' ? L'{' : 0;
            if (opening && std::count(candidate.begin(), candidate.end(), opening) >=
                std::count(candidate.begin(), candidate.end(), last)) break;
            --linkEnd;
        }
        AddLink(details, message.substr(start, linkEnd - start), L"");
        start = end;
    }
}

NotificationDetails ReadMetadata(const JsonObject& object)
{
    NotificationDetails details;
    details.priority = static_cast<int>(Number(object, L"priority", 5));
    details.time = Number(object, L"time", 253402300799);
    details.topic = String(object, L"topic");
    const auto tags = Read(object, L"tags", JsonValueType::Array);
    if (tags) {
        for (const auto& value : tags.GetArray()) {
            if (value.ValueType() == JsonValueType::String && !value.GetString().empty()) {
                details.tags.emplace_back(value.GetString());
            }
        }
    }
    return details;
}
}

bool IsWebLink(std::wstring_view url)
{
    if (url.size() < 8 || url.size() > 4096 ||
        url.find_first_of(L" \t\r\n\\") != std::wstring_view::npos ||
        std::any_of(url.begin(), url.end(), [](wchar_t character) { return character < 0x20; })) return false;
    try {
        const winrt::Windows::Foundation::Uri uri(url);
        const auto scheme = uri.SchemeName();
        return (scheme == L"http" || scheme == L"https") && !uri.Host().empty() &&
            uri.UserName().empty() && uri.Password().empty();
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

NotificationDetails IncomingDetails(const JsonObject& event, const std::wstring& topic, const std::wstring& text)
{
    auto details = ReadMetadata(event);
    if (!details.priority) details.priority = 3;
    if (details.topic.empty()) details.topic = topic;

    AddLink(details, String(event, L"click"), L"Open link");
    const auto actions = Read(event, L"actions", JsonValueType::Array);
    if (actions) {
        for (const auto& value : actions.GetArray()) {
            if (value.ValueType() != JsonValueType::Object) continue;
            const auto action = value.GetObject();
            if (String(action, L"action") == L"view") {
                AddLink(details, String(action, L"url"), String(action, L"label"));
            }
        }
    }
    const auto attachment = Read(event, L"attachment", JsonValueType::Object);
    if (attachment) {
        const auto object = attachment.GetObject();
        details.attachment = String(object, L"name");
        const auto type = String(object, L"type");
        if (details.attachment.empty()) details.attachment = type;
        else if (!type.empty()) details.attachment += L" (" + type + L")";
        AddLink(details, String(object, L"url"), L"Open attachment");
    }
    AddMessageLinks(details, text);
    return details;
}

NotificationDetails ReadSavedDetails(const JsonObject& reminder)
{
    const auto item = Read(reminder, L"details", JsonValueType::Object);
    if (!item) return {};
    const auto object = item.GetObject();
    auto details = ReadMetadata(object);
    details.attachment = String(object, L"attachment");
    const auto links = Read(object, L"links", JsonValueType::Array);
    if (links) {
        for (const auto& value : links.GetArray()) {
            if (value.ValueType() != JsonValueType::Object) continue;
            const auto link = value.GetObject();
            AddLink(details, String(link, L"url"), String(link, L"label"));
        }
    }
    return details;
}

JsonObject WriteDetails(const NotificationDetails& details)
{
    JsonObject object;
    object.Insert(L"priority", JsonValue::CreateNumberValue(details.priority));
    object.Insert(L"time", JsonValue::CreateNumberValue(static_cast<double>(details.time)));
    object.Insert(L"topic", JsonValue::CreateStringValue(details.topic));
    object.Insert(L"attachment", JsonValue::CreateStringValue(details.attachment));
    JsonArray tags;
    for (const auto& tag : details.tags) tags.Append(JsonValue::CreateStringValue(tag));
    object.Insert(L"tags", tags);
    JsonArray links;
    for (const auto& link : details.links) {
        JsonObject item;
        item.Insert(L"label", JsonValue::CreateStringValue(link.label));
        item.Insert(L"url", JsonValue::CreateStringValue(link.url));
        links.Append(item);
    }
    object.Insert(L"links", links);
    return object;
}
