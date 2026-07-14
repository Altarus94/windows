/*
** Dark mode support for the win wrapper library.
** Added for the Taiga "Dark Edition" custom build.
**
** Uses the undocumented uxtheme dark-mode APIs available since
** Windows 10 1809 (build 17763), with the 1903+ (18362) signatures.
*/

#pragma once

#include <windows.h>

namespace win {
namespace dark {

// Palette indices resolved by SysColor()/SysBrush() when dark mode is on.
// Any other index falls through to ::GetSysColor().
//
// COLOR_WINDOW        -> window/list background
// COLOR_WINDOWTEXT    -> primary text
// COLOR_3DFACE        -> panel background (sidebar, dialogs)
// COLOR_BTNFACE       -> same as 3DFACE
// COLOR_GRAYTEXT      -> secondary text
// COLOR_HIGHLIGHT     -> accent
// COLOR_ACTIVEBORDER  -> border lines
// COLOR_3DLIGHT       -> raised edge (subtle)
// COLOR_3DHIGHLIGHT   -> raised edge (strong)

bool Enabled();

// Process-wide initialization. Call once, before any window is created.
void Initialize();

// Dark title bar + dark-mode hint for a top-level window.
void ApplyToTopLevel(HWND hwnd);

// Walks child windows and applies per-class dark theming
// (ListView, TreeView, Header, Edit, ComboBox, Button, ScrollBar...).
void ApplyToChildren(HWND hwnd);

// Applies dark theming to a single control based on its class.
void ApplyToControl(HWND hwnd);

// Dark-aware replacement for ::GetSysColor().
COLORREF SysColor(int index);

// Cached solid brush for SysColor(index).
HBRUSH SysBrush(int index);

// Extra palette entries that have no system color equivalent.
COLORREF FieldColor();   // edit/combo field background
COLORREF HotColor();     // hover background
COLORREF AltRowColor();  // zebra-striping alternate row background

// Central WM_CTLCOLOR* handler. Returns an HBRUSH (as INT_PTR) when the
// message was handled, 0 otherwise.
INT_PTR HandleCtlColor(UINT uMsg, WPARAM wParam, LPARAM lParam);

// NM_CUSTOMDRAW handlers for controls that ignore theming.
// Return 0 when dark mode is off (caller falls back to default drawing).
LRESULT HandleToolbarCustomDraw(LPARAM lParam);  // LPNMTBCUSTOMDRAW
LRESULT HandleRebarCustomDraw(LPARAM lParam);    // LPNMCUSTOMDRAW

// Central WM_NOTIFY fallback: if this notification is an NM_CUSTOMDRAW from a
// toolbar (or rebar), dark-draws it and returns true with *result set. Lets
// every dialog's toolbars go dark without per-dialog wiring.
bool HandleNotifyCustomDraw(LPARAM lParam, LRESULT* result);

// Fully owner-draws a native ListView group header so its caption is legible
// on a dark background (the list ignores NM_CUSTOMDRAW clrText for groups).
// Call at CDDS_ITEMPREPAINT when NMLVCUSTOMDRAW.dwItemType == LVCDI_GROUP,
// passing the group id (nmcd.dwItemSpec). Returns CDRF_SKIPDEFAULT on success.
LRESULT DrawListGroupHeader(HWND list, HDC hdc, int group_id);

// Replacement for EnableWindow() on list views. A disabled list view repaints
// with light system colors that no theme or color override can fix, so in
// dark mode the list stays enabled and is dimmed instead.
void EnableListView(HWND hwnd, bool enable);

}  // namespace dark
}  // namespace win
