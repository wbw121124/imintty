#ifndef WINTAB_H
#define WINTAB_H

#include "container.h"

#define TABBARCLASS "Tabbar"

// Legacy tabbar functions (maintained for backward compat)
extern bool win_tabbar_visible();
extern void win_prepare_tabbar();
extern void win_open_tabbar();
extern void win_update_tabbar();
extern void win_close_tabbar();

#endif

