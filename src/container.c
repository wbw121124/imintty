// container.c (part of imintty) - P5 multi-tab container + panes
// Container window with tab strip, embedding child imintty processes via SetParent.

#include "winpriv.h"
#include "container.h"
#if CYGWIN_VERSION_API_MINOR < 74
#include "charset.h"
#endif

#ifdef MINGW_NATIVE
#include <windowsx.h>
#else
#include <w32api/windowsx.h>
#endif
#include <stdio.h>
#include <string.h>

#define TABSTRIP_HEIGHT 28
#define TAB_MIN_WIDTH 60
#define TAB_MAX_WIDTH 200

// Container state
static HWND container_wnd = 0;
static HWND active_embed_wnd = 0;
static HFONT tab_font = 0;
static bool container_initialized = false;
static int tab_strip_height = TABSTRIP_HEIGHT;

// Tab data
static HWND * embed_wnds = 0;
static int nembed = 0;
static int capembed = 0;

// Embed mode: child process knows it should create a child window
static bool embed_mode = false;

// Rendering sleep: inactive tabs skip update scheduling
static bool tab_render_active = true;

// Custom messages for container-child communication
#define CM_EMBED_ACTIVATE     (WM_USER + 200)
#define CM_EMBED_DEACTIVATE   (WM_USER + 201)
#define CM_EMBED_RESIZE       (WM_USER + 202)

// Forward declarations
static void container_tab_add(HWND wnd, const wchar * title);
static void container_tab_remove(HWND wnd);
static void container_tab_activate(HWND wnd);
static void container_tabbar_update(void);
static void container_create_tabbar_font(void);
static int  container_tab_width(void);
static void container_fit_title(HDC dc, int tab_width, wchar * title_in, wchar * title_out, int n);
static LRESULT CALLBACK container_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static void
container_init(void)
{
  if (container_initialized)
    return;

  RegisterClassExW(&(WNDCLASSEXW){
    .cbSize = sizeof(WNDCLASSEXW),
    .style = CS_HREDRAW | CS_VREDRAW,
    .lpfnWndProc = container_proc,
    .cbClsExtra = 0,
    .cbWndExtra = 0,
    .hInstance = inst,
    .hIcon = NULL,
    .hCursor = LoadCursor(NULL, IDC_ARROW),
    .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1),
    .lpszMenuName = NULL,
    .lpszClassName = CONTAINERCLASS,
  });

  container_initialized = true;
}

static void
container_destroy_all(void)
{
  if (!container_initialized)
    return;

  if (container_wnd) {
    DestroyWindow(container_wnd);
    container_wnd = 0;
  }

  for (int i = 0; i < nembed; i++) {
    if (embed_wnds[i]) {
      LONG style = GetWindowLong(embed_wnds[i], GWL_STYLE);
      style &= ~WS_CHILD;
      style |= WS_OVERLAPPEDWINDOW;
      SetWindowLong(embed_wnds[i], GWL_STYLE, style);
      SetParent(embed_wnds[i], NULL);
    }
  }
  nembed = 0;
  container_initialized = false;
}

static void
container_tab_add(HWND wnd, const wchar * title)
{
  (void)title;
  if (!wnd)
    return;

  for (int i = 0; i < nembed; i++) {
    if (embed_wnds[i] == wnd)
      return;
  }

  if (nembed >= capembed) {
    capembed = nembed ? nembed * 2 : 4;
    embed_wnds = renewn(embed_wnds, capembed);
  }
  embed_wnds[nembed++] = wnd;

  LONG style = GetWindowLong(wnd, GWL_STYLE);
  style &= ~WS_OVERLAPPEDWINDOW;
  style |= WS_CHILD | WS_CLIPCHILDREN;
  SetWindowLong(wnd, GWL_STYLE, style);

  LONG exstyle = GetWindowLong(wnd, GWL_EXSTYLE);
  exstyle &= ~WS_EX_LAYERED;
  exstyle &= ~WS_EX_TOOLWINDOW;
  SetWindowLong(wnd, GWL_EXSTYLE, exstyle);

  SetParent(wnd, container_wnd);
  SetWindowPos(wnd, NULL, 0, tab_strip_height, 0, 0,
               SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);

  PostMessage(wnd, CM_EMBED_DEACTIVATE, 0, 0);
  container_tabbar_update();
}

static void
container_tab_remove(HWND wnd)
{
  if (!wnd)
    return;

  for (int i = 0; i < nembed; i++) {
    if (embed_wnds[i] == wnd) {
      for (int j = i; j < nembed - 1; j++)
        embed_wnds[j] = embed_wnds[j + 1];
      nembed--;
      if (nembed > 0 && nembed < capembed / 2)
        embed_wnds = renewn(embed_wnds, max(nembed, 1));
      else
        embed_wnds = renewn(embed_wnds, nembed);

      LONG style = GetWindowLong(wnd, GWL_STYLE);
      style &= ~WS_CHILD;
      style |= WS_OVERLAPPEDWINDOW;
      SetWindowLong(wnd, GWL_STYLE, style);
      SetParent(wnd, NULL);

      container_tabbar_update();
      return;
    }
  }
}

static void
container_tab_activate(HWND target)
{
  if (target == active_embed_wnd)
    return;

  HWND prev = active_embed_wnd;
  active_embed_wnd = target;

  if (prev && prev != target) {
    PostMessage(prev, CM_EMBED_DEACTIVATE, 0, 0);
    LONG style = GetWindowLong(prev, GWL_STYLE);
    style &= ~WS_VSCROLL;
    SetWindowLong(prev, GWL_STYLE, style);
    SetWindowPos(prev, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }

  if (target) {
    LONG style = GetWindowLong(target, GWL_STYLE);
    style |= WS_VSCROLL;
    SetWindowLong(target, GWL_STYLE, style);
    SetWindowPos(target, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);

    PostMessage(target, CM_EMBED_ACTIVATE, 0, 0);
    SetForegroundWindow(target);
    SetFocus(target);
  }

  container_tabbar_update();
}

static int
container_tab_width(void)
{
  if (!container_wnd || nembed <= 0)
    return TAB_MIN_WIDTH;

  RECT cr;
  GetClientRect(container_wnd, &cr);
  int avail = cr.right - cr.left;
  int w = (avail - 8) / nembed;
  return max(TAB_MIN_WIDTH, min(w, TAB_MAX_WIDTH));
}

static void
container_fit_title(HDC dc, int tab_width, wchar * title_in, wchar * title_out, int n)
{
  int len = wcslen(title_in);
  if (len == 0) {
    title_out[0] = 0;
    return;
  }
  if (len <= 2) {
    wcsncpy(title_out, title_in, n - 1);
    title_out[n - 1] = 0;
    return;
  }

  SIZE sz;
  GetTextExtentPoint32W(dc, title_in, len, &sz);
  if (sz.cx <= tab_width - 8) {
    wcsncpy(title_out, title_in, n - 1);
    title_out[n - 1] = 0;
    return;
  }

  title_out[0] = title_in[0];
  title_out[1] = L'\u2026';
  title_out[2] = 0;
  GetTextExtentPoint32W(dc, title_out, 2, &sz);
  int cw = sz.cx;
  int ii;
  for (ii = len - 1; ii > 1; ii--) {
    int charw;
    GetCharWidth32W(dc, title_in[ii], title_in[ii], &charw);
    if (cw + charw <= tab_width - 8) {
      cw += charw;
      title_out[2 + (len - 1 - ii)] = title_in[ii];
    } else
      break;
  }
  title_out[2 + (len - 1 - ii)] = 0;
}

static void
container_create_tabbar_font(void)
{
  if (tab_font)
    DeleteObject(tab_font);
  tab_font = 0;

  if (*cfg.tab_font)
    tab_font = CreateFontW(cell_height * 9 / 10, cell_width * 9 / 10,
                           0, 0, FW_DONTCARE, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           DEFAULT_QUALITY, FIXED_PITCH | FF_DONTCARE,
                           cfg.tab_font);
  if (!tab_font)
    tab_font = CreateFontW(cell_height * 9 / 10, cell_width * 9 / 10,
                           0, 0, FW_DONTCARE, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           DEFAULT_QUALITY, FIXED_PITCH | FF_DONTCARE,
                           cfg.font.name);
}

static void
container_tabbar_update(void)
{
  if (!container_wnd || !container_initialized)
    return;
  InvalidateRect(container_wnd, NULL, TRUE);
  UpdateWindow(container_wnd);
}

static LRESULT CALLBACK
container_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg) {
    case WM_CREATE:
      container_create_tabbar_font();
      return 0;

    case WM_DESTROY:
      if (tab_font) {
        DeleteObject(tab_font);
        tab_font = 0;
      }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);

      RECT cr;
      GetClientRect(hwnd, &cr);

      // Background
      HBRUSH bg_brush = CreateSolidBrush(RGB(240, 240, 240));
      RECT strip_r = {0, 0, cr.right, tab_strip_height};
      FillRect(hdc, &strip_r, bg_brush);
      DeleteObject(bg_brush);

      // Separator line
      HPEN pen = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
      HGDIOBJ old_pen = SelectObject(hdc, pen);
      MoveToEx(hdc, 0, tab_strip_height - 1, NULL);
      LineTo(hdc, cr.right, tab_strip_height - 1);
      SelectObject(hdc, old_pen);
      DeleteObject(pen);

      // Tabs
      int tw = container_tab_width();
      HDC tabdc = hdc;
      HFONT old_font = (HFONT)SelectObject(tabdc, tab_font ?: (HFONT)GetStockObject(DEFAULT_GUI_FONT));

      wchar title_buf[256];
      for (int i = 0; i < nembed; i++) {
        RECT tab_r = {4 + i * tw, 2, 4 + (i + 1) * tw, tab_strip_height - 2};

        int tlen = GetWindowTextLengthW(embed_wnds[i]);
        wchar ttitle[tlen + 1];
        GetWindowTextW(embed_wnds[i], ttitle, tlen + 1);
        strip_title(ttitle);

        container_fit_title(tabdc, tw - 8, ttitle, title_buf, lengthof(title_buf));

        bool is_active = (embed_wnds[i] == active_embed_wnd);
        RECT fill_r = tab_r;

        if (is_active) {
          HBRUSH tab_brush = CreateSolidBrush(RGB(30, 30, 30));
          FillRect(tabdc, &fill_r, tab_brush);
          DeleteObject(tab_brush);
          SetTextColor(tabdc, RGB(255, 255, 255));
        } else {
          HBRUSH tab_brush = CreateSolidBrush(RGB(220, 220, 220));
          FillRect(tabdc, &fill_r, tab_brush);
          DeleteObject(tab_brush);
          SetTextColor(tabdc, RGB(50, 50, 50));
        }

        HPEN bp = CreatePen(PS_SOLID, 1, is_active ? RGB(30, 30, 30) : RGB(160, 160, 160));
        HGDIOBJ old_bp = SelectObject(tabdc, bp);
        SelectObject(tabdc, GetStockObject(NULL_BRUSH));
        Rectangle(tabdc, fill_r.left, fill_r.top, fill_r.right, fill_r.bottom);
        SelectObject(tabdc, old_bp);
        DeleteObject(bp);

        SetBkMode(tabdc, TRANSPARENT);
        int tx = tab_r.left + (tw - 8) / 2;
        int ty = tab_r.top + 6;
        TextOutW(tabdc, tx, ty, title_buf, wcslen(title_buf));
      }

      SelectObject(tabdc, old_font);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_SIZE: {
      RECT cr;
      GetClientRect(hwnd, &cr);
      int w = cr.right - cr.left;
      int h = cr.bottom - cr.top - tab_strip_height;
      for (int i = 0; i < nembed; i++) {
        if (embed_wnds[i] && IsWindow(embed_wnds[i])) {
          SetWindowPos(embed_wnds[i], NULL, 0, tab_strip_height, w, max(h, 1),
                       SWP_NOACTIVATE | SWP_NOZORDER);
        }
      }
      container_tabbar_update();
      return 0;
    }

    case WM_LBUTTONDOWN: {
      POINT p;
      p.x = GET_X_LPARAM(lp);
      p.y = GET_Y_LPARAM(lp);
      if (p.y >= 0 && p.y < tab_strip_height && nembed > 0) {
        int tw = container_tab_width();
        int idx = (p.x - 4) / tw;
        if (idx < 0) idx = 0;
        if (idx >= nembed) idx = nembed - 1;
        if (idx >= 0 && idx < nembed && embed_wnds[idx])
          container_tab_activate(embed_wnds[idx]);
      }
      return 0;
    }

    case WM_RBUTTONDOWN: {
      POINT p;
      p.x = GET_X_LPARAM(lp);
      p.y = GET_Y_LPARAM(lp);
      if (p.y >= 0 && p.y < tab_strip_height && nembed > 0) {
        int tw = container_tab_width();
        int idx = (p.x - 4) / tw;
        if (idx < 0) idx = 0;
        if (idx >= nembed) idx = nembed - 1;
        if (idx >= 0 && idx < nembed && embed_wnds[idx]) {
          HMENU hmenu = CreatePopupMenu();
          if (hmenu) {
            AppendMenuW(hmenu, MF_STRING, 1001, L"New Tab");
            if (nembed > 1)
              AppendMenuW(hmenu, MF_STRING, 1002, L"Close Tab");
            else
              AppendMenuW(hmenu, MF_GRAYED | MF_STRING, 1002, L"Close Tab");
            AppendMenuW(hmenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hmenu, MF_STRING, 1003, L"Close Others");

            POINT cp;
            GetCursorPos(&cp);
            int cmd = TrackPopupMenu(hmenu, TPM_RETURNCMD | TPM_NONOTIFY,
                                     cp.x, cp.y, 0, hwnd, NULL);
            if (cmd == 1001) {
              if (active_embed_wnd)
                PostMessage(active_embed_wnd, WM_USER, 0, WIN_NEW_TAB);
            } else if (cmd == 1002) {
              container_tab_remove(embed_wnds[idx]);
            } else if (cmd == 1003) {
              HWND keep = embed_wnds[idx];
              for (int j = nembed - 1; j >= 0; j--) {
                if (embed_wnds[j] != keep)
                  container_tab_remove(embed_wnds[j]);
              }
            }
            DestroyMenu(hmenu);
          }
        }
      }
      return 0;
    }

    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

bool
container_is_embedded(HWND wnd)
{
  return container_wnd && IsChild(container_wnd, wnd);
}

HWND
container_get_wnd(void)
{
  return container_wnd;
}

HWND
container_get_active(void)
{
  return active_embed_wnd;
}

void
container_set_embed_mode(HWND parent_hwnd)
{
  (void)parent_hwnd;
  embed_mode = true;
}

bool
container_is_embed_mode(void)
{
  return embed_mode;
}

bool
container_tab_should_render(void)
{
  return tab_render_active;
}

void
container_on_tab_close(HWND wnd)
{
  if (active_embed_wnd == wnd)
    active_embed_wnd = 0;
  container_tab_remove(wnd);

  if (nembed == 0) {
    container_destroy_all();
  } else if (active_embed_wnd == 0 && nembed > 0) {
    container_tab_activate(embed_wnds[0]);
  }
}

void
container_on_tab_create(HWND wnd, const wchar * title)
{
  (void)title;
  if (!container_wnd) {
    container_init();
    container_wnd = CreateWindowExW(0, CONTAINERCLASS, NULL,
                                    WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    NULL, NULL, inst, NULL);
    if (!container_wnd)
      return;
    ShowWindow(container_wnd, SW_SHOW);
  }

  container_tab_add(wnd, title);

  if (nembed == 1)
    container_tab_activate(wnd);
}

void
container_create_for_window(HWND wnd, const wchar * title)
{
  (void)title;
  container_init();

  if (!container_wnd) {
    container_wnd = CreateWindowExW(0, CONTAINERCLASS, NULL,
                                    WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    NULL, NULL, inst, NULL);
    if (!container_wnd)
      return;
    ShowWindow(container_wnd, SW_SHOW);
  }

  container_tab_add(wnd, title);
  container_tab_activate(wnd);
}
