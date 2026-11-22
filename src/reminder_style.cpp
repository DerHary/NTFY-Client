#include "reminder_style.hpp"
#include "resource.h"

#include <commctrl.h>
#include <richedit.h>
#include <windowsx.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>

namespace {
constexpr wchar_t ThemeProperty[] = L"NTFY.Client.ReminderTheme";

struct Palette {
    COLORREF surface, panel, line, text, muted, accent, accentPressed, accentText;
};

struct Theme : Palette {
    HBRUSH surfaceBrush = nullptr;
    HBRUSH panelBrush = nullptr;

    explicit Theme(Palette colors) : Palette(colors)
    {
        HIGHCONTRASTW setting{};
        setting.cbSize = sizeof(setting);
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(setting), &setting, 0) &&
            (setting.dwFlags & HCF_HIGHCONTRASTON)) {
            surface = panel = GetSysColor(COLOR_WINDOW);
            line = text = muted = GetSysColor(COLOR_WINDOWTEXT);
            accent = accentPressed = GetSysColor(COLOR_HIGHLIGHT);
            accentText = GetSysColor(COLOR_HIGHLIGHTTEXT);
        }
        surfaceBrush = CreateSolidBrush(surface);
        panelBrush = CreateSolidBrush(panel);
    }

    ~Theme()
    {
        DeleteObject(panelBrush);
        DeleteObject(surfaceBrush);
    }
};

const Palette& FixedColors(ReminderTheme theme)
{
    static constexpr Palette themes[] = {
        // White & Blue
        {RGB(249, 252, 255), RGB(235, 245, 255), RGB(170, 195, 223), RGB(20, 48, 82),
            RGB(75, 101, 132), RGB(27, 96, 191), RGB(18, 75, 163), RGB(255, 255, 255)},
        // Midnight Mint
        {RGB(19, 28, 37), RGB(27, 41, 51), RGB(55, 77, 88), RGB(238, 247, 247),
            RGB(171, 194, 201), RGB(135, 236, 199), RGB(106, 206, 170), RGB(13, 37, 33)},
        // Slate & Amber
        {RGB(35, 31, 28), RGB(48, 40, 34), RGB(94, 77, 66), RGB(255, 250, 241),
            RGB(215, 194, 173), RGB(255, 198, 113), RGB(232, 171, 85), RGB(49, 33, 18)},
        // Rose Quartz
        {RGB(255, 245, 246), RGB(252, 225, 232), RGB(204, 148, 164), RGB(67, 28, 45),
            RGB(118, 64, 83), RGB(151, 33, 80), RGB(120, 20, 61), RGB(255, 255, 255)},
        // Sea Glass
        {RGB(245, 252, 245), RGB(218, 243, 228), RGB(139, 190, 164), RGB(20, 57, 45),
            RGB(57, 103, 84), RGB(0, 106, 84), RGB(0, 79, 63), RGB(255, 255, 255)},
        // Lavender Ink
        {RGB(41, 27, 56), RGB(60, 40, 78), RGB(117, 86, 146), RGB(252, 245, 255),
            RGB(212, 190, 232), RGB(222, 178, 255), RGB(198, 148, 242), RGB(48, 20, 70)},
        // Deep Ocean
        {RGB(8, 31, 54), RGB(13, 53, 84), RGB(54, 112, 146), RGB(237, 250, 255),
            RGB(157, 206, 232), RGB(76, 214, 250), RGB(38, 182, 224), RGB(6, 39, 59)},
        // Graphite & Lime
        {RGB(24, 26, 21), RGB(36, 41, 29), RGB(92, 103, 69), RGB(247, 250, 239),
            RGB(197, 209, 178), RGB(195, 240, 84), RGB(163, 209, 56), RGB(28, 39, 10)},
    };
    static_assert(ARRAYSIZE(themes) == static_cast<size_t>(ReminderTheme::FollowWindows));
    static_assert(ARRAYSIZE(themes) + 1 == ARRAYSIZE(ReminderThemes));
    const auto index = static_cast<size_t>(theme);
    return themes[index < ARRAYSIZE(themes) ? index : 0];
}

COLORREF Mix(COLORREF base, COLORREF tint, int percent)
{
    const auto channel = [percent](int from, int to) {
        return (from * (100 - percent) + to * percent + 50) / 100;
    };
    return RGB(channel(GetRValue(base), GetRValue(tint)), channel(GetGValue(base), GetGValue(tint)),
        channel(GetBValue(base), GetBValue(tint)));
}

double Luminance(COLORREF color)
{
    const auto linear = [](BYTE channel) {
        const double value = channel / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(GetRValue(color)) + 0.7152 * linear(GetGValue(color)) +
        0.0722 * linear(GetBValue(color));
}

Palette WindowsColors()
{
    using namespace winrt::Windows::UI::ViewManagement;
    try {
        const UISettings settings;
        const auto color = [&](UIColorType type) {
            const auto value = settings.GetColorValue(type);
            return RGB(value.R, value.G, value.B);
        };
        const auto surface = color(UIColorType::Background);
        const auto text = color(UIColorType::Foreground);
        const bool dark = Luminance(text) > Luminance(surface);
        const auto panel = Mix(surface, text, dark ? 11 : 6);
        const auto muted = Mix(surface, text, 72);
        const auto line = Mix(surface, text, 32);

        // The same accent is used for links and for the main action. Keep both readable.
        auto accent = text;
        const auto panelLight = Luminance(panel) + 0.05;
        for (auto shade : {UIColorType::Accent, dark ? UIColorType::AccentLight1 : UIColorType::AccentDark1,
                 dark ? UIColorType::AccentLight2 : UIColorType::AccentDark2,
                 dark ? UIColorType::AccentLight3 : UIColorType::AccentDark3}) {
            const auto candidate = color(shade);
            const auto accentLight = Luminance(candidate) + 0.05;
            if ((dark ? accentLight / panelLight : panelLight / accentLight) >= 4.5) {
                accent = candidate;
                break;
            }
        }
        constexpr COLORREF white = RGB(255, 255, 255), black = RGB(0, 0, 0);
        const bool lightAccent = Luminance(accent) >= 0.179;
        return {surface, panel, line, text, muted, accent, Mix(accent, lightAccent ? white : black, 18),
            lightAccent ? black : white};
    } catch (const winrt::hresult_error&) {
        return FixedColors(ReminderTheme::WhiteBlue);
    }
}

const Theme& Colors(HWND window)
{
    if (const auto* theme = reinterpret_cast<const Theme*>(GetPropW(window, ThemeProperty))) return *theme;
    static const Theme fallback(FixedColors(ReminderTheme::WhiteBlue));
    return fallback;
}

int Scale(HWND window, int pixels)
{
    return MulDiv(pixels, GetDpiForWindow(window), 96);
}

void Rounded(HDC dc, const RECT& rect, int diameter, COLORREF fill, COLORREF line)
{
    const HPEN pen = CreatePen(PS_SOLID, 1, line);
    const HBRUSH brush = CreateSolidBrush(fill);
    const auto previousPen = SelectObject(dc, pen);
    const auto previousBrush = SelectObject(dc, brush);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, diameter, diameter);
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void Label(HDC dc, HWND control, const wchar_t* label, RECT bounds, COLORREF color, UINT alignment)
{
    const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    const auto previousFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, label, -1, &bounds, alignment | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (previousFont) SelectObject(dc, previousFont);
}

void Chevron(HDC dc, int x, int y, int size, COLORREF color)
{
    const HPEN pen = CreatePen(PS_SOLID, 1, color);
    const auto old = SelectObject(dc, pen);
    const POINT points[] = {{x - size, y - 1}, {x, y - 1 + size}, {x + size, y - 1}};
    Polyline(dc, points, ARRAYSIZE(points));
    SelectObject(dc, old);
    DeleteObject(pen);
}

void DrawComboFace(HWND combo)
{
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(combo, &paint);
    RECT bounds{};
    GetClientRect(combo, &bounds);
    const auto& colors = Colors(GetParent(combo));
    FillRect(dc, &bounds, colors.surfaceBrush);
    const bool focused = GetFocus() == combo;
    Rounded(dc, bounds, Scale(combo, 8), colors.panel, focused ? colors.accent : colors.line);

    wchar_t text[64]{};
    const auto index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (index >= 0) SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text));
    RECT label = bounds;
    label.left += Scale(combo, 9);
    label.right -= Scale(combo, 19);
    Label(dc, combo, text, label, colors.text, DT_LEFT | DT_END_ELLIPSIS);
    Chevron(dc, bounds.right - Scale(combo, 12), bounds.bottom / 2, Scale(combo, 4), colors.muted);
    EndPaint(combo, &paint);
}

LRESULT CALLBACK ComboProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_PAINT) {
        DrawComboFace(window);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    const auto result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == CB_SETCURSEL) {
        InvalidateRect(window, nullptr, FALSE);
    }
    return result;
}

void DrawControl(const DRAWITEMSTRUCT& item)
{
    const auto& colors = Colors(GetParent(item.hwndItem));
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    wchar_t label[64]{};
    RECT bounds = item.rcItem;
    if (item.CtlType == ODT_COMBOBOX) {
        const HBRUSH brush = selected ? CreateSolidBrush(colors.accent) : colors.panelBrush;
        FillRect(item.hDC, &bounds, brush);
        if (selected) DeleteObject(brush);
        if (item.itemID != UINT(-1)) {
            SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(label));
        }
        bounds.left += Scale(item.hwndItem, 8);
        Label(item.hDC, item.hwndItem, label, bounds, selected ? colors.accentText : colors.text, DT_LEFT);
        return;
    }

    const bool primary = item.CtlID == IDC_REMINDER_AGAIN;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    FillRect(item.hDC, &bounds, colors.surfaceBrush);
    const auto fill = primary ? (selected ? colors.accentPressed : colors.accent) : colors.panel;
    Rounded(item.hDC, bounds, Scale(item.hwndItem, 10), fill,
        primary ? fill : (focused ? colors.accent : colors.line));
    GetWindowTextW(item.hwndItem, label, ARRAYSIZE(label));
    Label(item.hDC, item.hwndItem, label, bounds, primary ? colors.accentText : colors.text, DT_CENTER);
    if (focused && primary) {
        InflateRect(&bounds, -3, -3);
        DrawFocusRect(item.hDC, &bounds);
    }
}

void DrawCard(HWND window)
{
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window, &paint);
    const auto& colors = Colors(window);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, colors.surfaceBrush);

    RECT accent{15, 15, 22, 24};
    MapDialogRect(window, &accent);
    Rounded(dc, accent, Scale(window, 5), colors.accent, colors.accent);
    RECT message{14, 56, 256, 135};
    MapDialogRect(window, &message);
    Rounded(dc, message, Scale(window, 12), colors.panel, colors.line);

    RECT divider{14, 183, 256, 183};
    MapDialogRect(window, &divider);
    divider.bottom = divider.top + 1;
    const auto line = CreateSolidBrush(colors.line);
    FillRect(dc, &divider, line);
    DeleteObject(line);
    EndPaint(window, &paint);
}

std::wstring Inline(std::wstring text)
{
    std::replace_if(text.begin(), text.end(), [](wchar_t character) { return character < 0x20; }, L' ');
    return text;
}

std::wstring LocalTime(std::int64_t timestamp)
{
    if (!timestamp) return L"—";
    const __time64_t value = timestamp;
    std::tm local{};
    if (_localtime64_s(&local, &value)) return L"—";
    wchar_t formatted[32]{};
    std::wcsftime(formatted, ARRAYSIZE(formatted), L"%Y-%m-%d %H:%M", &local);
    return formatted;
}

struct TextStream {
    const char* next;
    size_t remaining;
};

DWORD CALLBACK ReadText(DWORD_PTR cookie, LPBYTE target, LONG maximum, LONG* actual)
{
    auto& input = *reinterpret_cast<TextStream*>(cookie);
    const auto count = std::min<size_t>(input.remaining, maximum);
    std::memcpy(target, input.next, count);
    input.next += count;
    input.remaining -= count;
    *actual = static_cast<LONG>(count);
    return 0;
}

void FormatText(HWND edit, DWORD scope, COLORREF color, bool link = false)
{
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_LINK | CFM_UNDERLINE;
    format.crTextColor = color;
    if (link) format.dwEffects = CFE_LINK | CFE_UNDERLINE;
    SendMessageW(edit, EM_SETCHARFORMAT, scope, reinterpret_cast<LPARAM>(&format));
}
}

void StyleReminderWindow(HWND window, ReminderTheme theme)
{
    auto colors = std::make_unique<Theme>(
        theme == ReminderTheme::FollowWindows ? WindowsColors() : FixedColors(theme));
    if (SetPropW(window, ThemeProperty, colors.get())) colors.release();
    RECT client{};
    GetClientRect(window, &client);
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    SetWindowLongPtrW(window, GWL_STYLE,
        style & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU));
    SetWindowLongPtrW(window, GWL_EXSTYLE,
        extended & ~static_cast<LONG_PTR>(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE));
    SetWindowPos(window, nullptr, 0, 0, client.right, client.bottom,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    for (const int id : {IDC_REMINDER_UNIT, IDC_REMINDER_AMOUNT}) {
        SetWindowSubclass(GetDlgItem(window, id), ComboProc, 1, 0);
    }
    RoundReminderWindow(window);
}

void RoundReminderWindow(HWND window)
{
    RECT bounds{};
    GetWindowRect(window, &bounds);
    const auto width = bounds.right - bounds.left;
    const auto height = bounds.bottom - bounds.top;
    RECT regionBounds{};
    if (GetWindowRgnBox(window, &regionBounds) != ERROR &&
        regionBounds.right == width && regionBounds.bottom == height) return;

    const auto diameter = Scale(window, 22);
    auto region = CreateRoundRectRgn(0, 0, width, height, diameter, diameter);
    if (region && !SetWindowRgn(window, region, TRUE)) DeleteObject(region);
}

std::vector<ReminderLinkRange> SetReminderBody(HWND window, const Notification& notification)
{
    const auto& details = notification.details;
    const wchar_t* priorities[] = {L"—", L"Min", L"Low", L"Default", L"High", L"Urgent"};
    std::wstring text;
    if (details.priority || details.time || !details.topic.empty()) {
        const int priority = std::clamp(details.priority, 0, 5);
        text = std::wstring(L"Priority: ") + priorities[priority] + L"  ·  Time: " + LocalTime(details.time) +
            L"  ·  Topic: " + (details.topic.empty() ? L"—" : Inline(details.topic)) + L"\r";
    }
    for (size_t i = 0; i < details.tags.size(); ++i) {
        if (i) text += L"  ·  ";
        text += Inline(details.tags[i]);
    }
    if (!details.tags.empty()) text += L'\r';

    std::vector<ReminderLinkRange> ranges;
    if (!details.attachment.empty()) text += L"Attachment: " + Inline(details.attachment);
    if (details.links.size() > 1) {
        for (size_t i = 0; i < details.links.size(); ++i) {
            if (i || !details.attachment.empty()) text += L"  ·  ";
            const auto& link = details.links[i];
            const auto first = static_cast<LONG>(text.size());
            text += link.label.empty() ? link.url : Inline(link.label);
            ranges.push_back({first, static_cast<LONG>(text.size()), i});
        }
    }
    if (!text.empty() && text.back() != L'\r') text += L'\r';
    if (!text.empty() && !notification.text.empty()) text += L'\r';
    for (size_t i = 0; i < notification.text.size(); ++i) {
        const auto character = notification.text[i];
        if (character == L'\n' && i && notification.text[i - 1] == L'\r') continue;
        text += character == L'\n' ? L'\r' : character;
    }

    const HWND edit = GetDlgItem(window, IDC_REMINDER_TEXT);
    const auto& colors = Colors(window);
    SendMessageW(edit, EM_SETBKGNDCOLOR, FALSE, colors.panel);
    SendMessageW(edit, EM_SETEVENTMASK, 0, ENM_LINK);
    TextStream input{reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t)};
    EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&input), 0, ReadText};
    SendMessageW(edit, EM_STREAMIN, SF_TEXT | SF_UNICODE, reinterpret_cast<LPARAM>(&stream));
    FormatText(edit, SCF_ALL, colors.text);
    for (const auto& range : ranges) {
        CHARRANGE selection{range.first, range.end};
        SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        FormatText(edit, SCF_SELECTION, colors.accent, true);
    }
    SendMessageW(edit, EM_SETSEL, 0, 0);
    RECT bounds{};
    SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&bounds));
    const auto dc = GetDC(edit);
    if (!dc) return ranges;
    const auto font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    const auto old = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    if (old) SelectObject(dc, old);
    ReleaseDC(edit, dc);
    const auto lines = SendMessageW(edit, EM_GETLINECOUNT, 0, 0);
    ShowScrollBar(edit, SB_VERT, lines * metrics.tmHeight > bounds.bottom - bounds.top);
    return ranges;
}

INT_PTR PaintReminderWindow(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_SIZE) RoundReminderWindow(window);
    if (message == WM_NCDESTROY) delete reinterpret_cast<Theme*>(RemovePropW(window, ThemeProperty));
    if (message == WM_PAINT) {
        DrawCard(window);
        return TRUE;
    }
    if (message == WM_ERASEBKGND) return TRUE;
    if (message == WM_DRAWITEM) {
        DrawControl(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
        return TRUE;
    }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        const auto dc = reinterpret_cast<HDC>(wparam);
        const auto id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
        const auto& colors = Colors(window);
        const bool panel = id == IDC_REMINDER_TEXT || message == WM_CTLCOLORLISTBOX;
        const bool muted = id == IDC_REMINDER_BRAND || id == IDC_REMINDER_QUEUE ||
            id == IDC_REMINDER_HINT;
        SetTextColor(dc, muted ? colors.muted : colors.text);
        SetBkColor(dc, panel ? colors.panel : colors.surface);
        SetBkMode(dc, message == WM_CTLCOLORSTATIC && !panel ? TRANSPARENT : OPAQUE);
        return reinterpret_cast<INT_PTR>(panel ? colors.panelBrush : colors.surfaceBrush);
    }
    return FALSE;
}
