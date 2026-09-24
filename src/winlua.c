/*
 * Lua configuration engine.
 * Option LuaConfig points at a script (e.g. ~/.config/imintty/init.lua).
 *
 * API:
 *   imintty.set(key, value)   set option (UTF-8); returns bool
 *   imintty.get(key)          get option as string, or nil
 *   imintty.rc(path)          load another rc/theme file
 *   imintty.on(event, fn)     register event handler (config_loaded, bell, ...)
 *   imintty.command(name, fn) register named command for key/menu dispatch
 *   imintty.key(keyspec, act) append KeyFunctions binding
 *   imintty.expand_path(p)    expand ~ / convert Windows path to POSIX
 *   imintty.home              home directory (POSIX)
 */

#include <errno.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "config.h"
#include "charset.h"
#include "win.h"    // home
#include "winlua.h"

static lua_State * lua_st;
static int lua_depth;
static bool lua_config_loaded;

static void
lua_report(string where, string msg)
{
  fprintf(stderr, "imintty: lua %s: %s\n", where, msg ?: "(null)");
  fflush(stderr);
}

static char *
expand_tilde(string path)
{
  if (!path)
    return 0;
  if (path[0] == '~' && (path[1] == '/' || path[1] == '\\' || !path[1])) {
    if (!home)
      return 0;
    return asform("%s%s", home, path + 1);
  }
  return strdup(path);
}

static bool
is_win_abs_path(string p)
{
  if (!p || !p[0])
    return false;
  bool drive = ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))
               && p[1] == ':'
               && (p[2] == '\\' || p[2] == '/' || !p[2]);
  bool unc = p[0] == '\\' && p[1] == '\\';
  return drive || unc;
}

// Normalize to a path fopen can open under MSYS/Cygwin.
// Windows absolute paths convert to POSIX; POSIX paths pass through.
// Long paths: cygwin fopen handles > MAX_PATH in POSIX form.
char *
winlua_expand_path(string path)
{
  char *exp = expand_tilde(path);
  if (!exp)
    return 0;
  if (is_win_abs_path(exp)) {
    wchar *w = cs__utftowcs(exp);
    free(exp);
    if (!w)
      return 0;
    char *posix = path_win_w_to_posix(w);
    free(w);
    return posix;
  }
  return exp;
}

static int
l_set(lua_State *L)
{
  string key = luaL_checkstring(L, 1);
  string val = luaL_checkstring(L, 2);
  lua_pushboolean(L, config_set_option(key, val));
  return 1;
}

static int
l_get(lua_State *L)
{
  string key = luaL_checkstring(L, 1);
  char *val = config_get_option(key);
  if (val) {
    lua_pushstring(L, val);
    free(val);
  }
  else
    lua_pushnil(L);
  return 1;
}

static int
l_rc(lua_State *L)
{
  string path = luaL_checkstring(L, 1);
  char *ep = winlua_expand_path(path);
  if (!ep || !*ep) {
    free(ep);
    lua_pushboolean(L, 0);
    return 1;
  }
  load_config(ep, false);
  free(ep);
  lua_pushboolean(L, 1);
  return 1;
}

static int
l_on(lua_State *L)
{
  string event = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);

  lua_getglobal(L, "__imintty_events");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "__imintty_events");
  }
  // stack: events
  lua_getfield(L, -1, event);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, event);
  }
  // stack: events, list
  int n = (int)lua_rawlen(L, -1);
  lua_pushvalue(L, 2);
  lua_seti(L, -2, n + 1);
  lua_pop(L, 2);
  return 0;
}

static int
l_command(lua_State *L)
{
  string name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_getglobal(L, "__imintty_commands");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "__imintty_commands");
  }
  lua_pushvalue(L, 2);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);
  return 0;
}

static int
l_key(lua_State *L)
{
  string keyspec = luaL_checkstring(L, 1);
  string action = luaL_checkstring(L, 2);
  if (strchr(keyspec, ':') || !*keyspec) {
    lua_pushboolean(L, 0);
    return 1;
  }
  char *entry = asform("%s:%s;", keyspec, action);
  char *cur = *cfg.key_commands ? cs__wcstoutf(cfg.key_commands) : 0;
  char *joined = asform("%s%s", cur ?: "", entry);
  wchar *wj = cs__utftowcs(joined);
  wstrset(&cfg.key_commands, wj);
  free(wj);
  free(joined);
  free(cur);
  free(entry);
  lua_pushboolean(L, 1);
  return 1;
}

static int
l_expand_path(lua_State *L)
{
  string path = luaL_checkstring(L, 1);
  char *ep = winlua_expand_path(path);
  if (ep) {
    lua_pushstring(L, ep);
    free(ep);
  }
  else
    lua_pushnil(L);
  return 1;
}

static const luaL_Reg imintty_funcs[] = {
  {"set", l_set},
  {"get", l_get},
  {"rc", l_rc},
  {"on", l_on},
  {"command", l_command},
  {"key", l_key},
  {"expand_path", l_expand_path},
  {null, null}
};

void
winlua_init(void)
{
  if (lua_st)
    return;
  lua_st = luaL_newstate();
  if (!lua_st) {
    lua_report("init", "luaL_newstate failed");
    return;
  }
  luaL_openlibs(lua_st);
  lua_newtable(lua_st);
  luaL_setfuncs(lua_st, imintty_funcs, 0);
  lua_pushstring(lua_st, home ?: "");
  lua_setfield(lua_st, -2, "home");
  lua_setglobal(lua_st, "imintty");
}

bool
winlua_active(void)
{
  return lua_st != 0;
}

// stack top: list table; call each element as function with no args
static bool
call_list_handlers(lua_State *L, string event)
{
  bool ran = false;
  int n = (int)lua_rawlen(L, -1);
  for (int i = 1; i <= n; i++) {
    lua_geti(L, -1, i);
    if (lua_isfunction(L, -1)) {
      if (lua_pcall(L, 0, 0, 0) == LUA_OK)
        ran = true;
      else
        lua_report(event, lua_tostring(L, -1));
    }
    else
      lua_pop(L, 1);
    // clear any leftover error string
    if (lua_isstring(L, -1) && !lua_istable(L, -1))
      lua_pop(L, 1);
  }
  return ran;
}

bool
winlua_fire(string event)
{
  if (!lua_st || !event || lua_depth > 16)
    return false;
  lua_depth++;
  bool ran = false;
  int top = lua_gettop(lua_st);
  lua_getglobal(lua_st, "__imintty_events");
  if (lua_istable(lua_st, -1)) {
    lua_getfield(lua_st, -1, event);
    if (lua_istable(lua_st, -1))
      ran = call_list_handlers(lua_st, event);
    lua_settop(lua_st, top);
  }
  else
    lua_settop(lua_st, top);
  lua_depth--;
  return ran;
}

bool
winlua_call_command(string name)
{
  if (!lua_st || !name || lua_depth > 16)
    return false;
  lua_depth++;
  bool ok = false;
  int top = lua_gettop(lua_st);
  lua_getglobal(lua_st, "__imintty_commands");
  if (lua_istable(lua_st, -1)) {
    lua_getfield(lua_st, -1, name);
    if (lua_isfunction(lua_st, -1)) {
      if (lua_pcall(lua_st, 0, 0, 0) == LUA_OK)
        ok = true;
      else
        lua_report(name, lua_tostring(lua_st, -1));
    }
  }
  lua_settop(lua_st, top);
  lua_depth--;
  return ok;
}

void
winlua_load_config(void)
{
  if (!lua_st || !*cfg.lua_config)
    return;

  char *raw = cs__wcstoutf(cfg.lua_config);
  char *path = winlua_expand_path(raw);
  free(raw);
  if (!path || !*path) {
    free(path);
    lua_report("config", "LuaConfig path expand failed");
    return;
  }

  FILE *f = fopen(path, "rb");
  if (!f) {
    lua_report(path, strerror(errno));
    free(path);
    return;
  }

  size_t cap = 4096, len = 0;
  char *buf = newn(char, cap);
  size_t n;
  while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
    len += n;
    if (len + 1 >= cap) {
      cap *= 2;
      buf = renewn(buf, cap);
    }
  }
  buf[ferror(f) ? len : len] = 0;
  bool read_err = ferror(f) != 0;
  fclose(f);
  if (read_err) {
    lua_report(path, "read error");
    free(buf);
    free(path);
    return;
  }

  int top = lua_gettop(lua_st);
  int status = luaL_loadbuffer(lua_st, buf, len, path);
  free(buf);
  if (status == LUA_OK)
    status = lua_pcall(lua_st, 0, 0, 0);
  if (status != LUA_OK)
    lua_report(path, lua_tostring(lua_st, -1));
  else
    lua_config_loaded = true;
  lua_settop(lua_st, top);
  free(path);
}

void
winlua_reload(void)
{
  if (!lua_st)
    return;
  lua_pushnil(lua_st);
  lua_setglobal(lua_st, "__imintty_events");
  lua_pushnil(lua_st);
  lua_setglobal(lua_st, "__imintty_commands");
  lua_config_loaded = false;
  winlua_load_config();
  if (lua_config_loaded)
    winlua_fire("config_loaded");
}

void
winlua_shutdown(void)
{
  if (lua_st) {
    lua_close(lua_st);
    lua_st = 0;
    lua_depth = 0;
    lua_config_loaded = false;
  }
}
