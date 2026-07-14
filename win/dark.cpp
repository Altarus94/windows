/*
** Dark mode support for the win wrapper library.
** Added for the Taiga "Dark Edition" custom build.
**
** Techniques based on ysc3839/win32-darkmode and Notepad++'s NppDarkMode.
*/

#include <map>
#include <vector>

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <richedit.h>
#include <uxtheme.h>
#include <vssym32.h>

#include "dark.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")

namespace win {
namespace dark {

namespace {

// Palette
constexpr COLORREF kWindow      = RGB(0x20, 0x24, 0x2B);  // list/background
constexpr COLORREF kPanel       = RGB(0x26, 0x2B, 0x33);  // sidebar, dialogs
constexpr COLORREF kText        = RGB(0xE2, 0xE6, 0xEB);
constexpr COLORREF kGrayText    = RGB(0x94, 0x9C, 0xA6);
constexpr COLORREF kHighlight   = RGB(0x2D, 0x7D, 0xB3);  // selection accent
constexpr COLORREF kBorder      = RGB(0x16, 0x19, 0x1E);
constexpr COLORREF k3DLight     = RGB(0x39, 0x40, 0x4B);
constexpr COLORREF k3DHighlight = RGB(0x44, 0x4C, 0x58);
constexpr COLORREF kField       = RGB(0x2B, 0x31, 0x3A);  // edit boxes
constexpr COLORREF kHot         = RGB(0x33, 0x3A, 0x45);
constexpr COLORREF kAltRow      = RGB(0x2A, 0x2F, 0x38);  // zebra alt row
constexpr COLORREF kGroupHeader = RGB(0x89, 0xC4, 0xFF);  // list group caption

constexpr UINT_PTR kTabSubclassId = 0xDA01;
constexpr UINT_PTR kStatusBarSubclassId = 0xDA02;
constexpr UINT_PTR kListViewSubclassId = 0xDA03;
constexpr UINT_PTR kButtonSubclassId = 0xDA04;
constexpr UINT_PTR kUpDownSubclassId = 0xDA05;
constexpr UINT_PTR kDateTimeSubclassId = 0xDA06;

bool enabled = false;

std::map<COLORREF, HBRUSH> brushes;

HBRUSH GetCachedBrush(COLORREF color) {
  auto it = brushes.find(color);
  if (it != brushes.end())
    return it->second;
  HBRUSH brush = ::CreateSolidBrush(color);
  brushes[color] = brush;
  return brush;
}

// Undocumented uxtheme dark-mode APIs (Windows 10 1903+)
enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };

using fnSetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode);  // ordinal 135
using fnAllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);                 // ordinal 133
using fnRefreshImmersiveColorPolicyState = void(WINAPI*)();                 // ordinal 104
using fnFlushMenuThemes = void(WINAPI*)();                                  // ordinal 136

fnSetPreferredAppMode _SetPreferredAppMode = nullptr;
fnAllowDarkModeForWindow _AllowDarkModeForWindow = nullptr;
fnRefreshImmersiveColorPolicyState _RefreshImmersiveColorPolicyState = nullptr;
fnFlushMenuThemes _FlushMenuThemes = nullptr;

bool IsWindows10DarkModeCapable() {
  // RtlGetVersion reports the true build regardless of manifest
  using fnRtlGetVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  auto ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (!ntdll)
    return false;
  auto rtl_get_version = reinterpret_cast<fnRtlGetVersion>(
      ::GetProcAddress(ntdll, "RtlGetVersion"));
  if (!rtl_get_version)
    return false;
  RTL_OSVERSIONINFOW info = {sizeof(info)};
  if (rtl_get_version(&info) != 0)
    return false;
  return info.dwMajorVersion > 10 ||
         (info.dwMajorVersion == 10 && info.dwBuildNumber >= 18362);
}

bool IsHighContrast() {
  HIGHCONTRASTW hc = {sizeof(hc)};
  if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, FALSE))
    return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
  return false;
}

////////////////////////////////////////////////////////////////////////////////
// Tab control: no dark theme class exists, so it is owner-painted

void PaintTab(HWND hwnd, HDC hdc) {
  RECT rect_client = {0};
  ::GetClientRect(hwnd, &rect_client);
  ::FillRect(hdc, &rect_client, GetCachedBrush(kPanel));

  HFONT font = reinterpret_cast<HFONT>(::SendMessageW(hwnd, WM_GETFONT, 0, 0));
  HGDIOBJ old_font = ::SelectObject(hdc, font);
  ::SetBkMode(hdc, TRANSPARENT);

  const int count = TabCtrl_GetItemCount(hwnd);
  const int selected = TabCtrl_GetCurSel(hwnd);

  for (int i = 0; i < count; ++i) {
    RECT rect_item = {0};
    TabCtrl_GetItemRect(hwnd, i, &rect_item);

    wchar_t text[128] = {0};
    TCITEMW item = {0};
    item.mask = TCIF_TEXT;
    item.pszText = text;
    item.cchTextMax = _countof(text) - 1;
    TabCtrl_GetItem(hwnd, i, &item);

    if (i == selected) {
      ::FillRect(hdc, &rect_item, GetCachedBrush(kHot));
      // Accent line along the bottom edge of the selected tab
      RECT rect_accent = rect_item;
      rect_accent.top = rect_accent.bottom - 2;
      ::FillRect(hdc, &rect_accent, GetCachedBrush(kHighlight));
      ::SetTextColor(hdc, kText);
    } else {
      ::SetTextColor(hdc, kGrayText);
    }

    RECT rect_text = rect_item;
    rect_text.bottom -= 2;
    ::DrawTextW(hdc, text, -1, &rect_text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  ::SelectObject(hdc, old_font);
}

LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                                 LPARAM lParam, UINT_PTR, DWORD_PTR) {
  switch (uMsg) {
    case WM_ERASEBKGND:
      return TRUE;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = ::BeginPaint(hwnd, &ps);
      PaintTab(hwnd, hdc);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, TabSubclassProc, kTabSubclassId);
      break;
  }
  return ::DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

////////////////////////////////////////////////////////////////////////////////
// Status bar: no dark theme class exists, so it is owner-painted

void PaintStatusBar(HWND hwnd, HDC hdc) {
  RECT rect_client = {0};
  ::GetClientRect(hwnd, &rect_client);
  ::FillRect(hdc, &rect_client, GetCachedBrush(kPanel));

  // Top border line
  RECT rect_border = rect_client;
  rect_border.bottom = rect_border.top + 1;
  ::FillRect(hdc, &rect_border, GetCachedBrush(kBorder));

  HFONT font = reinterpret_cast<HFONT>(::SendMessageW(hwnd, WM_GETFONT, 0, 0));
  HGDIOBJ old_font = ::SelectObject(hdc, font);
  ::SetBkMode(hdc, TRANSPARENT);
  ::SetTextColor(hdc, kText);

  const int parts = static_cast<int>(::SendMessageW(hwnd, SB_GETPARTS, 0, 0));
  for (int i = 0; i < parts; ++i) {
    RECT rect_part = {0};
    ::SendMessageW(hwnd, SB_GETRECT, i, reinterpret_cast<LPARAM>(&rect_part));

    int text_left = rect_part.left + 4;

    HICON icon = reinterpret_cast<HICON>(::SendMessageW(hwnd, SB_GETICON, i, 0));
    if (icon) {
      const int icon_size = ::GetSystemMetrics(SM_CXSMICON);
      const int icon_y =
          rect_part.top + ((rect_part.bottom - rect_part.top) - icon_size) / 2;
      ::DrawIconEx(hdc, text_left, icon_y, icon, icon_size, icon_size, 0,
                   nullptr, DI_NORMAL);
      text_left += icon_size + 4;
    }

    const LRESULT text_info = ::SendMessageW(hwnd, SB_GETTEXTLENGTHW, i, 0);
    const int text_length = LOWORD(text_info);
    if (text_length > 0) {
      std::vector<wchar_t> text(static_cast<size_t>(text_length) + 1, L'\0');
      ::SendMessageW(hwnd, SB_GETTEXTW, i, reinterpret_cast<LPARAM>(text.data()));
      RECT rect_text = rect_part;
      rect_text.left = text_left;
      rect_text.right -= 2;
      ::DrawTextW(hdc, text.data(), -1, &rect_text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                  DT_END_ELLIPSIS);
    }
  }

  ::SelectObject(hdc, old_font);
}

LRESULT CALLBACK StatusBarSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                                       LPARAM lParam, UINT_PTR, DWORD_PTR) {
  switch (uMsg) {
    case WM_ERASEBKGND:
      return TRUE;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = ::BeginPaint(hwnd, &ps);
      PaintStatusBar(hwnd, hdc);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, StatusBarSubclassProc, kStatusBarSubclassId);
      break;
  }
  return ::DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

////////////////////////////////////////////////////////////////////////////////
// ListView: the DarkMode_ItemsView header theme keeps near-black text, so the
// header text color is fixed via NM_CUSTOMDRAW. Group-view headers are NOT sent
// through custom draw at all, so they are repainted over the default drawing.

void RepaintGroupHeaders(HWND list) {
  if (!enabled)
    return;
  if (!::SendMessageW(list, LVM_ISGROUPVIEWENABLED, 0, 0))
    return;
  const int count = static_cast<int>(::SendMessageW(list, LVM_GETGROUPCOUNT, 0, 0));
  if (count <= 0)
    return;

  HDC hdc = ::GetDC(list);
  if (!hdc)
    return;
  for (int i = 0; i < count; ++i) {
    LVGROUP group = {0};
    group.cbSize = sizeof(group);
    group.mask = LVGF_GROUPID;
    if (::SendMessageW(list, LVM_GETGROUPINFOBYINDEX, i,
                       reinterpret_cast<LPARAM>(&group)))
      DrawListGroupHeader(list, hdc, group.iGroupId);
  }
  ::ReleaseDC(list, hdc);
}

LRESULT CALLBACK ListViewSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                                      LPARAM lParam, UINT_PTR, DWORD_PTR) {
  switch (uMsg) {
    case WM_NOTIFY: {
      auto header = reinterpret_cast<LPNMHDR>(lParam);
      if (header->code == NM_CUSTOMDRAW &&
          header->hwndFrom == ListView_GetHeader(hwnd)) {
        auto custom_draw = reinterpret_cast<LPNMCUSTOMDRAW>(lParam);
        switch (custom_draw->dwDrawStage) {
          case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
          case CDDS_ITEMPREPAINT:
            ::SetTextColor(custom_draw->hdc, kText);
            return CDRF_NEWFONT;
        }
      }
      break;
    }
    case WM_PAINT: {
      // Paint normally, then overpaint the (unreadable) group headers
      LRESULT result = ::DefSubclassProc(hwnd, uMsg, wParam, lParam);
      RepaintGroupHeaders(hwnd);
      return result;
    }
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, ListViewSubclassProc, kListViewSubclassId);
      break;
  }
  return ::DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

////////////////////////////////////////////////////////////////////////////////
// Up-down (spinner) buttons: no dark theme class exists, owner-painted

void PaintUpDownArrow(HDC hdc, const RECT& rect, bool up) {
  const int cx = (rect.left + rect.right) / 2;
  const int cy = (rect.top + rect.bottom) / 2;
  POINT points[3];
  if (up) {
    points[0] = {cx - 3, cy + 2};
    points[1] = {cx + 3, cy + 2};
    points[2] = {cx, cy - 2};
  } else {
    points[0] = {cx - 3, cy - 2};
    points[1] = {cx + 3, cy - 2};
    points[2] = {cx, cy + 2};
  }
  HGDIOBJ old_brush = ::SelectObject(hdc, GetCachedBrush(kGrayText));
  HGDIOBJ old_pen = ::SelectObject(hdc, ::GetStockObject(NULL_PEN));
  ::Polygon(hdc, points, 3);
  ::SelectObject(hdc, old_pen);
  ::SelectObject(hdc, old_brush);
}

LRESULT CALLBACK UpDownSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                                    LPARAM lParam, UINT_PTR, DWORD_PTR) {
  switch (uMsg) {
    case WM_ERASEBKGND:
      return TRUE;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = ::BeginPaint(hwnd, &ps);
      RECT rect_client = {0};
      ::GetClientRect(hwnd, &rect_client);
      // Blends with the edit field it is attached to
      ::FillRect(hdc, &rect_client, GetCachedBrush(kField));
      RECT rect_up = rect_client;
      rect_up.bottom = (rect_client.top + rect_client.bottom) / 2;
      RECT rect_down = rect_client;
      rect_down.top = rect_up.bottom;
      PaintUpDownArrow(hdc, rect_up, true);
      PaintUpDownArrow(hdc, rect_down, false);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, UpDownSubclassProc, kUpDownSubclassId);
      break;
  }
  return ::DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

////////////////////////////////////////////////////////////////////////////////
// Date-time pickers: no dark theme class exists, owner-painted
// (the drop-down month calendar remains light for now)

LRESULT CALLBACK DateTimeSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                                      LPARAM lParam, UINT_PTR, DWORD_PTR) {
  switch (uMsg) {
    case WM_ERASEBKGND:
      return TRUE;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = ::BeginPaint(hwnd, &ps);
      RECT rect_client = {0};
      ::GetClientRect(hwnd, &rect_client);

      ::FillRect(hdc, &rect_client, GetCachedBrush(kField));
      ::FrameRect(hdc, &rect_client, GetCachedBrush(k3DLight));

      const bool enabled = ::IsWindowEnabled(hwnd) != FALSE;
      int text_left = rect_client.left + 4;

      // DTS_SHOWNONE check box
      SYSTEMTIME time = {0};
      bool checked = true;
      if (::GetWindowLongW(hwnd, GWL_STYLE) & DTS_SHOWNONE) {
        checked = ::SendMessageW(hwnd, DTM_GETSYSTEMTIME, 0,
                                 reinterpret_cast<LPARAM>(&time)) == GDT_VALID;
        const int box = 13;
        RECT rect_box = {0};
        rect_box.left = text_left;
        rect_box.top = rect_client.top +
            ((rect_client.bottom - rect_client.top) - box) / 2;
        rect_box.right = rect_box.left + box;
        rect_box.bottom = rect_box.top + box;
        if (HTHEME theme = ::OpenThemeData(hwnd, L"Button")) {
          int state = checked ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL;
          if (!enabled)
            state = checked ? CBS_CHECKEDDISABLED : CBS_UNCHECKEDDISABLED;
          ::DrawThemeBackground(theme, hdc, BP_CHECKBOX, state, &rect_box,
                                nullptr);
          ::CloseThemeData(theme);
        }
        text_left = rect_box.right + 4;
      }

      // Formatted date text
      wchar_t text[128] = {0};
      ::GetWindowTextW(hwnd, text, _countof(text));
      HFONT font = reinterpret_cast<HFONT>(::SendMessageW(hwnd, WM_GETFONT, 0, 0));
      HGDIOBJ old_font = ::SelectObject(hdc, font);
      ::SetBkMode(hdc, TRANSPARENT);
      ::SetTextColor(hdc, (enabled && checked) ? kText : kGrayText);
      RECT rect_text = rect_client;
      rect_text.left = text_left;
      rect_text.right -= 16;
      ::DrawTextW(hdc, text, -1, &rect_text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      ::SelectObject(hdc, old_font);

      // Drop-down chevron
      RECT rect_arrow = rect_client;
      rect_arrow.left = rect_arrow.right - 16;
      PaintUpDownArrow(hdc, rect_arrow, false);

      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, DateTimeSubclassProc, kDateTimeSubclassId);
      break;
  }
  return ::DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

////////////////////////////////////////////////////////////////////////////////
// Group boxes / check boxes / radio buttons: the themed BUTTON keeps drawing
// its label in near-black, so these are owner-painted with a light label

void PaintGroupBox(HWND hwnd, HDC hdc) {
  RECT rect_client = {0};
  ::GetClientRect(hwnd, &rect_client);
  ::FillRect(hdc, &rect_client, GetCachedBrush(kPanel));

  wchar_t text[256] = {0};
  const int text_length = ::GetWindowTextW(hwnd, text, _countof(text));

  HFONT font = reinterpret_cast<HFONT>(::SendMessageW(hwnd, WM_GETFONT, 0, 0));
  HGDIOBJ old_font = ::SelectObject(hdc, font);

  SIZE text_size = {0};
  if (text_length > 0)
    ::GetTextExtentPoint32W(hdc, text, text_length, &text_size);

  // Frame starts at the vertical middle of the caption
  RECT rect_frame = rect_client;
  rect_frame.top += text_size.cy / 2;
  ::FrameRect(hdc, &rect_frame, GetCachedBrush(k3DLight));

  // Caption, over an erased gap in the top border
  if (text_length > 0) {
    RECT rect_text = {rect_client.left + 8, rect_client.top,
                      rect_client.left + 12 + text_size.cx, rect_client.top + text_size.cy};
    ::FillRect(hdc, &rect_text, GetCachedBrush(kPanel));
    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, ::IsWindowEnabled(hwnd) ? kText : kGrayText);
    rect_text.left += 2;
    ::DrawTextW(hdc, text, text_length, &rect_text,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
  }

  ::SelectObject(hdc, old_font);
}

void PaintCheckOrRadio(HWND hwnd, HDC hdc, bool radio, bool hot) {
  RECT rect_client = {0};
  ::GetClientRect(hwnd, &rect_client);
  ::FillRect(hdc, &rect_client, GetCachedBrush(kPanel));

  const bool disabled = !::IsWindowEnabled(hwnd);
  const LRESULT check = ::SendMessageW(hwnd, BM_GETCHECK, 0, 0);
  const int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;
  int base = check == BST_CHECKED ? 5 : (check == BST_INDETERMINATE ? 9 : 1);
  const int state_id = base + (disabled ? 3 : (hot ? 1 : 0));

  HTHEME theme = ::OpenThemeData(hwnd, L"Button");
  SIZE glyph = {0};
  if (theme)
    ::GetThemePartSize(theme, hdc, part, state_id, nullptr, TS_DRAW, &glyph);
  if (glyph.cx <= 0) { glyph.cx = 13; glyph.cy = 13; }

  RECT rect_glyph = {rect_client.left,
                     rect_client.top + (rect_client.bottom - rect_client.top - glyph.cy) / 2,
                     rect_client.left + glyph.cx,
                     rect_client.top + (rect_client.bottom - rect_client.top - glyph.cy) / 2 + glyph.cy};
  if (theme)
    ::DrawThemeBackground(theme, hdc, part, state_id, &rect_glyph, nullptr);
  if (theme)
    ::CloseThemeData(theme);

  wchar_t text[256] = {0};
  const int text_length = ::GetWindowTextW(hwnd, text, _countof(text));
  if (text_length > 0) {
    HFONT font = reinterpret_cast<HFONT>(::SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = ::SelectObject(hdc, font);
    RECT rect_text = rect_client;
    rect_text.left = rect_glyph.right + 4;
    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, disabled ? kGrayText : kText);
    ::DrawTextW(hdc, text, text_length, &rect_text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_WORDBREAK);
    ::SelectObject(hdc, old_font);
  }
}

LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                                    LPARAM lParam, UINT_PTR, DWORD_PTR data) {
  const LONG type = ::GetWindowLongW(hwnd, GWL_STYLE) & BS_TYPEMASK;
  const bool is_group = type == BS_GROUPBOX;

  switch (uMsg) {
    case WM_ERASEBKGND:
      return TRUE;
    case WM_MOUSEMOVE: {
      if (!is_group && !data) {
        ::SetWindowSubclass(hwnd, ButtonSubclassProc, kButtonSubclassId, 1);
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;
    }
    case WM_MOUSELEAVE: {
      if (!is_group && data) {
        ::SetWindowSubclass(hwnd, ButtonSubclassProc, kButtonSubclassId, 0);
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = ::BeginPaint(hwnd, &ps);
      if (is_group) {
        PaintGroupBox(hwnd, hdc);
      } else {
        const bool radio =
            type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
        PaintCheckOrRadio(hwnd, hdc, radio, data != 0);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, ButtonSubclassProc, kButtonSubclassId);
      break;
  }
  return ::DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

BOOL CALLBACK ApplyToControlProc(HWND hwnd, LPARAM) {
  ApplyToControl(hwnd);
  return TRUE;
}

}  // namespace

bool Enabled() {
  return enabled;
}

void Initialize() {
  if (!IsWindows10DarkModeCapable() || IsHighContrast())
    return;

  auto uxtheme = ::LoadLibraryExW(L"uxtheme.dll", nullptr,
                                  LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!uxtheme)
    return;

  _SetPreferredAppMode = reinterpret_cast<fnSetPreferredAppMode>(
      ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
  _AllowDarkModeForWindow = reinterpret_cast<fnAllowDarkModeForWindow>(
      ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
  _RefreshImmersiveColorPolicyState = reinterpret_cast<fnRefreshImmersiveColorPolicyState>(
      ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(104)));
  _FlushMenuThemes = reinterpret_cast<fnFlushMenuThemes>(
      ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));

  if (!_SetPreferredAppMode)
    return;

  _SetPreferredAppMode(PreferredAppMode::ForceDark);
  if (_RefreshImmersiveColorPolicyState)
    _RefreshImmersiveColorPolicyState();
  if (_FlushMenuThemes)
    _FlushMenuThemes();

  enabled = true;
}

void ApplyToTopLevel(HWND hwnd) {
  if (!enabled || !hwnd)
    return;

  if (_AllowDarkModeForWindow)
    _AllowDarkModeForWindow(hwnd, true);

  // Dark title bar. 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (2004+),
  // 19 on older 1809-1909 builds.
  BOOL value = TRUE;
  if (FAILED(::DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value))))
    ::DwmSetWindowAttribute(hwnd, 19, &value, sizeof(value));
}

void ApplyToControl(HWND hwnd) {
  if (!enabled || !hwnd)
    return;

  wchar_t class_name[64] = {0};
  ::GetClassNameW(hwnd, class_name, _countof(class_name));

  if (_AllowDarkModeForWindow)
    _AllowDarkModeForWindow(hwnd, true);

  if (::lstrcmpiW(class_name, WC_LISTVIEWW) == 0) {
    ::SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
    ListView_SetBkColor(hwnd, kWindow);
    ListView_SetTextBkColor(hwnd, kWindow);
    ListView_SetTextColor(hwnd, kText);
    // Grid lines draw in a fixed light color; drop them on dark backgrounds
    ListView_SetExtendedListViewStyleEx(hwnd, LVS_EX_GRIDLINES, 0);
    if (HWND header = ListView_GetHeader(hwnd)) {
      if (_AllowDarkModeForWindow)
        _AllowDarkModeForWindow(header, true);
      ::SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
    }
    ::SetWindowSubclass(hwnd, ListViewSubclassProc, kListViewSubclassId, 0);
  } else if (::lstrcmpiW(class_name, WC_TREEVIEWW) == 0) {
    ::SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
    TreeView_SetBkColor(hwnd, kPanel);
    TreeView_SetTextColor(hwnd, kText);
    TreeView_SetLineColor(hwnd, k3DLight);
  } else if (::lstrcmpiW(class_name, WC_HEADERW) == 0) {
    ::SetWindowTheme(hwnd, L"DarkMode_ItemsView", nullptr);
  } else if (::lstrcmpiW(class_name, WC_EDITW) == 0) {
    ::SetWindowTheme(hwnd, L"DarkMode_CFD", nullptr);
  } else if (::lstrcmpiW(class_name, WC_COMBOBOXW) == 0) {
    ::SetWindowTheme(hwnd, L"DarkMode_CFD", nullptr);
    // Dark scrollbar for the drop-down list
    COMBOBOXINFO info = {sizeof(info)};
    if (::GetComboBoxInfo(hwnd, &info) && info.hwndList)
      ::SetWindowTheme(info.hwndList, L"DarkMode_Explorer", nullptr);
  } else if (::lstrcmpiW(class_name, WC_BUTTONW) == 0) {
    const LONG type = ::GetWindowLongW(hwnd, GWL_STYLE) & BS_TYPEMASK;
    switch (type) {
      case BS_GROUPBOX:
      case BS_CHECKBOX:
      case BS_AUTOCHECKBOX:
      case BS_3STATE:
      case BS_AUTO3STATE:
      case BS_RADIOBUTTON:
      case BS_AUTORADIOBUTTON:
        // Owner-painted so the label is legible on a dark background
        ::SetWindowSubclass(hwnd, ButtonSubclassProc, kButtonSubclassId, 0);
        break;
      default:
        // Push buttons theme correctly on their own
        ::SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
        break;
    }
  } else if (::lstrcmpiW(class_name, WC_SCROLLBARW) == 0) {
    ::SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
  } else if (::lstrcmpiW(class_name, STATUSCLASSNAMEW) == 0) {
    ::SetWindowSubclass(hwnd, StatusBarSubclassProc, kStatusBarSubclassId, 0);
  } else if (::lstrcmpiW(class_name, WC_TABCONTROLW) == 0) {
    ::SetWindowSubclass(hwnd, TabSubclassProc, kTabSubclassId, 0);
  } else if (::lstrcmpiW(class_name, UPDOWN_CLASSW) == 0) {
    ::SetWindowSubclass(hwnd, UpDownSubclassProc, kUpDownSubclassId, 0);
  } else if (::lstrcmpiW(class_name, DATETIMEPICK_CLASSW) == 0) {
    ::SetWindowSubclass(hwnd, DateTimeSubclassProc, kDateTimeSubclassId, 0);
  } else if (::lstrcmpiW(class_name, REBARCLASSNAMEW) == 0) {
    ::SendMessageW(hwnd, RB_SETBKCOLOR, 0, static_cast<LPARAM>(kPanel));
    ::SendMessageW(hwnd, RB_SETTEXTCOLOR, 0, static_cast<LPARAM>(kText));
  } else if (::lstrcmpiW(class_name, TOOLBARCLASSNAMEW) == 0) {
    // Kill the etched edges; button colors come from NM_CUSTOMDRAW
    COLORSCHEME scheme = {sizeof(scheme), kPanel, kPanel};
    ::SendMessageW(hwnd, TB_SETCOLORSCHEME, 0,
                   reinterpret_cast<LPARAM>(&scheme));
    HWND tooltips = reinterpret_cast<HWND>(
        ::SendMessageW(hwnd, TB_GETTOOLTIPS, 0, 0));
    if (tooltips)
      ::SetWindowTheme(tooltips, L"DarkMode_Explorer", nullptr);
  } else if (::lstrcmpiW(class_name, TOOLTIPS_CLASSW) == 0) {
    ::SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
  } else if (::lstrcmpiW(class_name, L"SysLink") == 0) {
    // Link text is recolored via NM_CUSTOMDRAW (HandleNotifyCustomDraw)
    ::SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
  } else if (::lstrcmpiW(class_name, L"RICHEDIT50W") == 0 ||
             ::lstrcmpiW(class_name, RICHEDIT_CLASSW) == 0) {
    ::SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
    ::SendMessageW(hwnd, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(kWindow));
    CHARFORMATW format = {0};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR;
    format.crTextColor = kText;
    ::SendMessageW(hwnd, EM_SETCHARFORMAT, SCF_ALL,
                   reinterpret_cast<LPARAM>(&format));
    ::SendMessageW(hwnd, EM_SETCHARFORMAT, SCF_DEFAULT,
                   reinterpret_cast<LPARAM>(&format));
  }
}

void ApplyToChildren(HWND hwnd) {
  if (!enabled || !hwnd)
    return;
  ::EnumChildWindows(hwnd, ApplyToControlProc, 0);
}

COLORREF SysColor(int index) {
  if (!enabled)
    return ::GetSysColor(index);

  switch (index) {
    case COLOR_WINDOW:        return kWindow;
    case COLOR_WINDOWTEXT:    return kText;
    case COLOR_BTNTEXT:       return kText;
    case COLOR_3DFACE:        return kPanel;  // == COLOR_BTNFACE
    case COLOR_GRAYTEXT:      return kGrayText;
    // Taiga uses COLOR_HIGHLIGHT as a TEXT accent (new-episode titles,
    // episode-count warnings); the selection blue is too dark to read on a
    // dark background, so return a brighter accent instead
    case COLOR_HIGHLIGHT:     return RGB(0x63, 0xB2, 0xF0);
    case COLOR_HIGHLIGHTTEXT: return kText;
    case COLOR_ACTIVEBORDER:  return kBorder;
    case COLOR_3DLIGHT:       return k3DLight;
    case COLOR_BTNHIGHLIGHT:  return k3DHighlight;  // == COLOR_3DHIGHLIGHT
    case COLOR_3DDKSHADOW:    return kBorder;
    case COLOR_BTNSHADOW:     return kBorder;       // == COLOR_3DSHADOW
    case COLOR_APPWORKSPACE:  return kBorder;
    default:                  return ::GetSysColor(index);
  }
}

HBRUSH SysBrush(int index) {
  if (!enabled)
    return ::GetSysColorBrush(index);
  return GetCachedBrush(SysColor(index));
}

COLORREF FieldColor() {
  return kField;
}

COLORREF HotColor() {
  return kHot;
}

COLORREF AltRowColor() {
  return kAltRow;
}

INT_PTR HandleCtlColor(UINT uMsg, WPARAM wParam, LPARAM lParam) {
  if (!enabled)
    return 0;

  HDC hdc = reinterpret_cast<HDC>(wParam);

  switch (uMsg) {
    case WM_CTLCOLORDLG:
      return reinterpret_cast<INT_PTR>(GetCachedBrush(kPanel));
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      ::SetTextColor(hdc, kText);
      ::SetBkColor(hdc, kPanel);
      ::SetBkMode(hdc, TRANSPARENT);
      return reinterpret_cast<INT_PTR>(GetCachedBrush(kPanel));
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      ::SetTextColor(hdc, kText);
      ::SetBkColor(hdc, kField);
      return reinterpret_cast<INT_PTR>(GetCachedBrush(kField));
    }
  }

  return 0;
}

LRESULT HandleToolbarCustomDraw(LPARAM lParam) {
  if (!enabled)
    return CDRF_DODEFAULT;

  auto custom_draw = reinterpret_cast<LPNMTBCUSTOMDRAW>(lParam);

  switch (custom_draw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: {
      RECT rect_client = {0};
      ::GetClientRect(custom_draw->nmcd.hdr.hwndFrom, &rect_client);
      ::FillRect(custom_draw->nmcd.hdc, &rect_client, GetCachedBrush(kPanel));
      return CDRF_NOTIFYITEMDRAW;
    }
    case CDDS_ITEMPREPAINT: {
      custom_draw->clrText = kText;
      custom_draw->clrTextHighlight = kText;
      custom_draw->clrBtnFace = kPanel;
      custom_draw->clrBtnHighlight = kPanel;
      custom_draw->clrHighlightHotTrack = kHot;
      custom_draw->nStringBkMode = TRANSPARENT;
      LRESULT result = TBCDRF_USECDCOLORS | TBCDRF_HILITEHOTTRACK |
                       TBCDRF_NOEDGES | TBCDRF_NOETCHEDEFFECT;
      // Draw pressed/open (dropdown) button states ourselves
      if (custom_draw->nmcd.uItemState & (CDIS_SELECTED | CDIS_CHECKED)) {
        ::FillRect(custom_draw->nmcd.hdc, &custom_draw->nmcd.rc,
                   GetCachedBrush(kHot));
        custom_draw->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_CHECKED);
        result |= TBCDRF_NOBACKGROUND;
      }
      return result;
    }
  }

  return CDRF_DODEFAULT;
}

LRESULT HandleRebarCustomDraw(LPARAM lParam) {
  if (!enabled)
    return CDRF_DODEFAULT;

  auto custom_draw = reinterpret_cast<LPNMCUSTOMDRAW>(lParam);

  if (custom_draw->dwDrawStage == CDDS_PREPAINT) {
    RECT rect_client = {0};
    ::GetClientRect(custom_draw->hdr.hwndFrom, &rect_client);
    ::FillRect(custom_draw->hdc, &rect_client, GetCachedBrush(kPanel));
    return CDRF_SKIPDEFAULT;
  }

  return CDRF_DODEFAULT;
}

LRESULT DrawListGroupHeader(HWND list, HDC hdc, int group_id) {
  if (!enabled)
    return CDRF_DODEFAULT;

  // Header rectangle for this group
  RECT rect = {0};
  rect.top = LVGGR_HEADER;
  if (!::SendMessageW(list, LVM_GETGROUPRECT, group_id,
                      reinterpret_cast<LPARAM>(&rect)))
    return CDRF_DODEFAULT;

  ::FillRect(hdc, &rect, GetCachedBrush(kWindow));

  wchar_t text[256] = {0};
  LVGROUP group = {0};
  group.cbSize = sizeof(group);
  group.mask = LVGF_HEADER;
  group.pszHeader = text;
  group.cchHeader = _countof(text);
  ::SendMessageW(list, LVM_GETGROUPINFO, group_id,
                 reinterpret_cast<LPARAM>(&group));
  const int text_length = ::lstrlenW(text);

  HFONT font = reinterpret_cast<HFONT>(::SendMessageW(list, WM_GETFONT, 0, 0));
  HGDIOBJ old_font = ::SelectObject(hdc, font);
  ::SetBkMode(hdc, TRANSPARENT);
  ::SetTextColor(hdc, kGroupHeader);

  SIZE text_size = {0};
  if (text_length > 0)
    ::GetTextExtentPoint32W(hdc, text, text_length, &text_size);

  RECT rect_text = rect;
  rect_text.left += 6;
  ::DrawTextW(hdc, text, text_length, &rect_text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

  // Divider line trailing the caption
  const int line_left = rect.left + 6 + text_size.cx + 8;
  const int line_y = (rect.top + rect.bottom) / 2;
  if (line_left < rect.right - 6) {
    RECT rect_line = {line_left, line_y, rect.right - 6, line_y + 1};
    ::FillRect(hdc, &rect_line, GetCachedBrush(k3DLight));
  }

  ::SelectObject(hdc, old_font);
  return CDRF_SKIPDEFAULT;
}

void EnableListView(HWND hwnd, bool enable) {
  if (!enabled) {
    ::EnableWindow(hwnd, enable);
    return;
  }
  // Keep the list enabled (a disabled one repaints with light system colors
  // no override can fix) and dim the text instead
  ::EnableWindow(hwnd, TRUE);
  ListView_SetTextColor(hwnd, enable ? kText : kGrayText);
  ::InvalidateRect(hwnd, nullptr, TRUE);
}

bool HandleNotifyCustomDraw(LPARAM lParam, LRESULT* result) {
  if (!enabled)
    return false;

  auto hdr = reinterpret_cast<LPNMHDR>(lParam);
  if (!hdr->hwndFrom)
    return false;

  // Date-time picker drop-down: the month calendar is created on demand and
  // ignores custom colors while themed, so untheme + recolor it as it opens
  if (hdr->code == DTN_DROPDOWN) {
    if (HWND monthcal = DateTime_GetMonthCal(hdr->hwndFrom)) {
      ::SetWindowTheme(monthcal, L"", L"");
      MonthCal_SetColor(monthcal, MCSC_BACKGROUND, kPanel);
      MonthCal_SetColor(monthcal, MCSC_MONTHBK, kField);
      MonthCal_SetColor(monthcal, MCSC_TEXT, kText);
      // Quirk: the day-of-week names are drawn with the TITLE BACKGROUND
      // color, so it must double as readable text on the dark month interior
      // (light slate band + dark title text)
      MonthCal_SetColor(monthcal, MCSC_TITLEBK, RGB(0x8A, 0x96, 0xA3));
      MonthCal_SetColor(monthcal, MCSC_TITLETEXT, RGB(0x1A, 0x1D, 0x23));
      MonthCal_SetColor(monthcal, MCSC_TRAILINGTEXT, kGrayText);

      // The classic (unthemed) calendar is taller than the themed one the
      // popup was sized for; resize the control and its host popup to match
      RECT rect_min = {0};
      MonthCal_GetMinReqRect(monthcal, &rect_min);
      int width = rect_min.right;
      const int width_today = MonthCal_GetMaxTodayWidth(monthcal);
      if (width_today > width)
        width = width_today;
      // A little breathing room: at the exact minimum size, the "Today"
      // line sits cramped against the popup border
      width += 6;
      const int height = rect_min.bottom + 8;
      ::SetWindowPos(monthcal, nullptr, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      if (HWND popup = ::GetParent(monthcal)) {
        RECT rect_window = {0}, rect_client = {0};
        ::GetWindowRect(popup, &rect_window);
        ::GetClientRect(popup, &rect_client);
        const int frame_width =
            (rect_window.right - rect_window.left) - rect_client.right;
        const int frame_height =
            (rect_window.bottom - rect_window.top) - rect_client.bottom;
        ::SetWindowPos(popup, nullptr, 0, 0, width + frame_width,
                       height + frame_height,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }
    return false;  // side effect only; let default processing continue
  }

  if (hdr->code != NM_CUSTOMDRAW)
    return false;

  wchar_t class_name[32] = {0};
  ::GetClassNameW(hdr->hwndFrom, class_name, _countof(class_name));

  if (::lstrcmpiW(class_name, TOOLBARCLASSNAMEW) == 0) {
    *result = HandleToolbarCustomDraw(lParam);
    return true;
  }
  if (::lstrcmpiW(class_name, REBARCLASSNAMEW) == 0) {
    *result = HandleRebarCustomDraw(lParam);
    return true;
  }
  if (::lstrcmpiW(class_name, L"SysLink") == 0) {
    // Recolor hyperlink text to a brighter blue legible on a dark background
    auto custom_draw = reinterpret_cast<LPNMCUSTOMDRAW>(lParam);
    switch (custom_draw->dwDrawStage) {
      case CDDS_PREPAINT:
        *result = CDRF_NOTIFYITEMDRAW;
        return true;
      case CDDS_ITEMPREPAINT:
        ::SetTextColor(custom_draw->hdc, kGroupHeader);
        *result = CDRF_NEWFONT;
        return true;
    }
    return false;
  }
  if (::lstrcmpiW(class_name, WC_LISTVIEWW) == 0) {
    // Safety net for plain list views that don't dark-draw their own rows
    // (e.g. the settings media list): force dark row background + light text.
    // Lists with their own NM_CUSTOMDRAW handle it before this fallback runs.
    auto custom_draw = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
    switch (custom_draw->nmcd.dwDrawStage) {
      case CDDS_PREPAINT:
        *result = CDRF_NOTIFYITEMDRAW;
        return true;
      case CDDS_ITEMPREPAINT:
        custom_draw->clrText = kText;
        custom_draw->clrTextBk = kWindow;
        *result = CDRF_NEWFONT;
        return true;
    }
    return false;
  }
  return false;
}

}  // namespace dark
}  // namespace win
