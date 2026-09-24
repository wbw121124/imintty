#ifndef WINLUA_H
#define WINLUA_H

// Lua configuration engine (LuaConfig option).
// Load after finish_config(); paths support ~/, POSIX, Windows, long paths.

extern void winlua_init(void);
extern void winlua_load_config(void);
extern void winlua_reload(void);
extern void winlua_shutdown(void);
extern bool winlua_active(void);

// Fire a named event to handlers registered with imintty.on(event, fn).
// Returns false if no handler ran or Lua is inactive.
extern bool winlua_fire(string event);

// Invoke a command registered with imintty.command(name, fn).
extern bool winlua_call_command(string name);

// Expand ~/ and normalize a path for fopen (POSIX or Windows).
// Returns a newly allocated path, or null on failure.
extern char * winlua_expand_path(string path);

#endif
