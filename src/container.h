#ifndef CONTAINER_H
#define CONTAINER_H

#include "win.h"

// Container window class name
#define CONTAINERCLASS L"IminttyContainer"

// Custom messages for container-child communication
#define CM_EMBED_ACTIVATE     (WM_USER + 200)
#define CM_EMBED_DEACTIVATE   (WM_USER + 201)
#define CM_EMBED_RESIZE       (WM_USER + 202)

// WIN_NEW_TAB is defined in winpriv.h

extern HWND container_get_wnd(void);
extern HWND container_get_active(void);
extern bool container_is_embedded(HWND wnd);
extern bool container_is_embed_mode(void);
extern void container_set_embed_mode(HWND parent_hwnd);
extern bool container_tab_should_render(void);
extern void container_on_tab_close(HWND wnd);
extern void container_on_tab_create(HWND wnd, const wchar * title);
extern void container_create_for_window(HWND wnd, const wchar * title);

#endif
