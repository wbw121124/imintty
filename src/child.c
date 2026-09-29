// child.c (part of imintty)
// Copyright 2008-11 Andy Koppe, 2015-2026 Thomas Wolff
// Licensed under the terms of the GNU General Public License v3 or later.

#include "child.h"

#include "term.h"
#include "charset.h"

#include "winpriv.h"  /* win_prefix_title, win_update_now */
#include "tek.h"      /* tek_mode for log filtering */
#include "appinfo.h"  /* APPNAME, VERSION */
#include "conpty.h"

#ifndef LOG_SILENT
#define LOG_SILENT 0
#define LOG_FAIL   1
#define LOG_ERROR  2
#define LOG_WARN   3
#define LOG_INFO   4
#define LOG_DEBUG  5
#define LOG_FULL   6
#endif
#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_SILENT
#endif
#define DBGx(lvl, ...) do { if (LOG_LEVEL >= (lvl)) fprintf(stderr, __VA_ARGS__); } while(0)

#if defined(__MINGW32__) || defined(__MINGW64__)
/* P8: MinGW-w64 — Windows-native APIs, no POSIX layer */
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <process.h>
#include <io.h>
#else
#include <pwd.h>
#include <fcntl.h>
#include <utmp.h>
#include <dirent.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#ifdef __CYGWIN__
#include <sys/cygwin.h>  // cygwin_internal
#endif

#if CYGWIN_VERSION_API_MINOR >= 93
#include <pty.h>
#else
int forkpty(int *, char *, struct termios *, struct winsize *);
#endif

#if CYGWIN_VERSION_DLL_MAJOR < 1007
#include <winnls.h>
#include <wincon.h>
#include <wingdi.h>
#include <winuser.h>
#endif

// exit code to use for failure of `exec` (changed from 255, see #745)
// http://www.tldp.org/LDP/abs/html/exitcodes.html
#define mexit 126
#endif /* !MINGW */

/* P8: MinGW-w64 compatibility helpers */
#if defined(__MINGW32__) || defined(__MINGW64__)
#include <process.h>
#include <io.h>

/* Minimal _getpid replacement */
static pid_t mingw_getpid(void) { return (pid_t)_getpid(); }

/* Minimal setenv/unsetenv via Windows API */
static int mingw_setenv(const char *name, const char *value, int overwrite)
{
  wchar_t wname[256], wvalue[4096];
  int len_name = MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);
  if (len_name <= 0) return -1;
  if (!overwrite) {
    wchar_t existing[4096];
    if (GetEnvironmentVariableW(wname, existing, 4096) > 0) return 0;
  }
  MultiByteToWideChar(CP_UTF8, 0, value, -1, wvalue, 4096);
  return SetEnvironmentVariableW(wname, wvalue) ? 0 : -1;
}
static void mingw_unsetenv(const char *name)
{
  wchar_t wname[256];
  MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);
  SetEnvironmentVariableW(wname, NULL);
}
#define setenv mingw_setenv
#define unsetenv mingw_unsetenv
#define getpid() mingw_getpid()

/* Toolhelp32-based process info (replaces /proc) */
static BOOL __attribute__((unused))
snapshot_process(HANDLE *hSnap, DWORD *pid_out, int max_pids)
{
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  *hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (*hSnap == INVALID_HANDLE_VALUE) return FALSE;
  if (!Process32FirstW(*hSnap, &pe)) { CloseHandle(*hSnap); return FALSE; }
  *pid_out = (DWORD)pe.th32ProcessID;
  int count = 1;
  while (Process32NextW(*hSnap, &pe) && count < max_pids) {
    pid_out[count++] = (DWORD)pe.th32ProcessID;
  }
  return TRUE;
}
#endif /* __MINGW32__ || __MINGW64__ */

string child_dir = null;

bool logging = false;
static pid_t pid;
static bool killed = false;
static int win_fd;
static int pty_fd = -1;
static int log_fd = -1;
/* ConPTY backend state */
static HPCON conpty_handle = null;
static HANDLE conpty_input = null;
static HANDLE conpty_output = null;
static bool use_conpty = false;
#if CYGWIN_VERSION_API_MINOR >= 74
static struct winsize prev_winsize = {0, 0, 0, 0};
#else
static struct winsize prev_winsize;
#endif

#if CYGWIN_VERSION_API_MINOR >= 66
#include <langinfo.h>
#endif


#define dont_debug_dir

#ifdef debug_dir
#define trace_dir(d)	show_info(d)
#else
#define trace_dir(d)	
#endif


static void
childerror(char * action, bool from_fork, int errno_code, int code)
{
#if CYGWIN_VERSION_API_MINOR >= 66
  bool utf8 = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;
  char * oldloc;
#else
  char * oldloc = (char *)cs_get_locale();
  bool utf8 = strstr(oldloc, ".65001");
#endif
  if (utf8)
    oldloc = null;
  else {
    oldloc = strdup(cs_get_locale());
    cs_set_locale("C.UTF-8");
  }

  char s[33];
  bool colour_code = code && !errno_code;
  bool new_line = code || errno_code;
  sprintf(s, "%s\033[30;%dm\033[K", new_line ? "\r\n" : "", from_fork ? 41 : colour_code ? code : 43);
  term_write(s, strlen(s));
  term_write(action, strlen(action));
  if (errno_code) {
    char * err = strerror(errno_code);
    if (from_fork && errno_code == ENOENT)
      err = _("There are no available terminals");
    term_write(": ", 2);
    term_write(err, strlen(err));
  }
  if (code && !colour_code) {
    sprintf(s, " (%d)", code);
    term_write(s, strlen(s));
  }
  term_write(".\033[0m\r\n", 7);

  if (oldloc) {
    cs_set_locale(oldloc);
    free(oldloc);
  }
}

static void
sigexit(int sig)
{
  if (pid)
    kill(-pid, SIGHUP);
  signal(sig, SIG_DFL);
  report_pos();
  kill(getpid(), sig);
}

static void
open_logfile(bool toggling)
{
  // Open log file if any
  if (*cfg.log) {
    // use cygwin conversion function to escape unencoded characters 
    // and thus avoid the locale trick (2.2.3)

    if (0 == wcscmp(cfg.log, W("-"))) {
      log_fd = fileno(stdout);
      logging = true;
    }
    else {
      char * log;
      if (*cfg.log == '~' && cfg.log[1] == '/') {
        // substitute '~' -> home
        char * path = cs__wcstombs(&cfg.log[2]);
        log = asform("%s/%s", home, path);
        free(path);
      }
      else
        log = path_win_w_to_posix(cfg.log);
#ifdef debug_logfilename
      printf("<%ls> -> <%s>\n", cfg.log, log);
#endif
      char * format = strchr(log, '%');
      if (format && * ++ format == 'd' && !strchr(format, '%')) {
        char * logf = newn(char, strlen(log) + 20);
        sprintf(logf, log, getpid());
        free(log);
        log = logf;
      }
      else if (format) {
        time_t nowt = time(0);
        char * logf = newn(char, MAX_PATH + 1);
        strftime(logf, MAX_PATH, log, localtime(& nowt));
        free(log);
        log = logf;
      }
      // also expand placeholders $h or $p with hostname or pid
      if ((format = strstr(log, "$h"))) {
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif
        char hostname[HOST_NAME_MAX + 1];
#ifdef MINGW_NATIVE
        if (0 == mingw_gethostname(hostname, HOST_NAME_MAX)) {
#else
        if (0 == gethostname(hostname, HOST_NAME_MAX)) {
#endif
          *format = '%'; *++format = 's';
          char * logf = asform(log, hostname);
          free(log);
          log = logf;
        }
      }
      if ((format = strstr(log, "$p"))) {
        *format = '%'; *++format = 'd';
        char * logf = asform(log, getpid());
        free(log);
        log = logf;
      }

      log_fd = open(log, O_WRONLY | O_CREAT | O_EXCL, 0600);
      if (log_fd < 0) {
        // report message and filename:
        wchar * wpath = path_posix_to_win_w(log);
        char * upath = cs__wcstoutf(wpath);
#ifdef debug_logfilename
        printf(" -> <%ls> -> <%s>\n", wpath, upath);
#endif
        char * msg = _("Error: Could not open log file");
        if (toggling) {
          char * err = strerror(errno);
          char * errmsg = newn(char, strlen(msg) + strlen(err) + strlen(upath) + 4);
          sprintf(errmsg, "%s: %s\n%s", msg, err, upath);
          win_show_warning(errmsg);
          free(errmsg);
        }
        else {
          childerror(msg, false, errno, 0);
          childerror(upath, false, 0, 0);
        }
        free(upath);
        free(wpath);
      }
      else
        logging = true;

      free(log);
    }
  }
}

void
toggle_logging()
{
  if (logging)
    logging = false;
  else if (log_fd >= 0)
    logging = true;
  else
    open_logfile(true);
}

#ifdef debug_log_filter
static void
printline(char * tag, char * s, int len)
{
  printf("[%s] [2m»[m", tag);
  while (len > 0) {
    if ((*s & 0xE0) == 0)
      printf("[7m^%c[m", *s + 0x40);
    else if (*s == 0x7F)
      printf("[7m^?[m");
    else
      printf("%c", *s);
    s ++;
    len --;
  }
  printf("[2m«[m\n");
}
#else
#define printline(tag, s, len)	
#endif

static void
term_log(char * s, uint len)
{
static char buf[999];
static uint bufi = 0;
static char state = 0;

  printline("log", s, len);

  void term_log_flush()
  {
    printline("log-flush", buf, bufi);
    write(log_fd, buf, bufi);
    bufi = 0;
  }

  void term_log_char(char c)
  {
    if (bufi >= sizeof(buf)) {
      term_log_flush();
      state = 0;
    }
    else if (c == '\e' && !state)
      term_log_flush();
    else {
      printline("not-flush", buf, bufi);
    }

    buf[bufi++] = c;

    if (term.vt52_mode || tek_mode)
      return;

    if (!state) {
      if (c == '') {
        bufi--;
        state = 0;
      }
      else if (c == '\e')
        state = 'e';
    }
    else if (state == 'e') {
      if (c == '[')
        state = 'c';
      else if (c == ']')
        state = 'o';
      else if (c == 'P')
        state = 'd';
      else if (c == 'Z') {
        bufi = 0;
        state = 0;
      }
    }
    else if (state == 'c') { // CSI (ESC [)
      if (c >= '@') { // CSI terminator
        state = 0;
        char * csi = &buf[2];
        char c0 = *csi, c1 = buf[bufi - 2], c2 = buf[bufi - 1];
        if (c0 >= '0' && c0 <= ';')
          c0 = 0;
        else
          csi ++;
        int num = 0, num2 = 0;
        sscanf(csi, "%u;%u", &num, &num2);
        sscanf(csi, "%u", &num);
        if ((c2 == 'n' && (!c0 || c0 == '?'))
            || (c2 == 'c' && !num && (!c0 || c0 == '>' || c0 == '='))
            || (c2 == 'q' && c0 == '>' && !num)
            || (c2 == 't' && !c0 && num >= 11 && num <= 21)
            || (c2 == 'p' && c1 == '$' && (!c0 || c0 == '?'))
            || (c2 == 'y' && c1 == '*' && !c0)
            || (c2 == 'v' && c1 == '"' && !c0)
            || (c2 == 'R' && c1 == '#' && !c0)
            || (c2 == 'x' && !c0 && num <= 1)
            || (c2 == 'w' && c1 == '$' && !c0)
            || (c2 == 'm' && c0 == '?')
            || (c2 == 'h' && c0 == '?' &&
                (num == 9 || (num >= 1000 && num <= 1004) || num == 1007
                 || num == 7786 || num == 7787
                )
               )
            || (c2 == 'w' && c1 == '\'' && !c0)
            || (c2 == 'z' && c1 == '\'' && !c0)
            || (c2 == '|' && c1 == '\'' && !c0)
            || (c2 == 'S' && c1 == '#' && !c0)
            || (c2 == '|' && c1 == '#' && !c0)
            || (c2 == 'S' && c0 == '?' && (num2 == 1 || num2 == 4))
           )
          bufi = 0;
#ifdef debug_log_filter
        if (!bufi)
          printf("log filter CSI %c %d %d %c %c\n", c0 ?: '-', num, num2, c1, c2);
#endif
      }
    }
    else if (state == 'o') { // OSC (ESC ])
      if (c == '' || (c == '\\' && buf[bufi - 1] == '\e')) { // ST or BEL
        state = 0;
        int num = -1, numq1 = -1, numq2 = -1;
        int slen = 0;
        if (2 == sscanf(&buf[2], "%u;%u;%n", &num, &numq2, &slen)) {
          if (buf[2 + slen] != '?')
            numq2 = -1;
        }
        else {
          sscanf(&buf[2], "%u;%n", &num, &slen);
          if (buf[2 + slen] == '?')
            numq1 = num;
        }
#ifdef debug_log_filter
        int bufend = bufi;
#endif
        if (numq2 == 4 || numq2 == 7704 || numq2 == 5)
          bufi = 0;
        else if ((numq1 >= 10 && numq1 <= 19) || numq1 == 50 || numq1 == 52
                 || numq1 == 701 || numq1 == 7770 || numq1 == 7771 || numq1 == 7777
                )
          bufi = 0;
        else if (num == 60 || num == 61 || num == 62)
          bufi = 0;
#ifdef debug_log_filter
        if (!bufi) {
          buf[bufend] = 0;
          printf("log filter OSC %d %d %d (\\e%s)\n", num, numq1, numq2, &buf[1]);
        }
#endif
      }
    }
    else if (state == 'd') { // DCS (ESC P)
      if (c == '\\' && buf[bufi - 1] == '\e') { // ST
        state = 0;
        if (!strncmp(&buf[2], "$q", 2) || !strncasecmp(&buf[2], "+q", 2))
          bufi = 0;
      }
    }
  }

  if (log_fd >= 0 && logging) {
    if (!cfg.log_filter)
      write(log_fd, s, len);
    else {
      for (uint i = 0; i < len; i++)
        term_log_char(s[i]);
    }
  }
}

void
child_close_log(void)
{
  // Cleanup ConPTY resources
  if (use_conpty) {
    if (conpty_input) {
      CloseHandle(conpty_input);
      conpty_input = null;
    }
    if (conpty_output) {
      CloseHandle(conpty_output);
      conpty_output = null;
    }
    if (conpty_handle) {
      conpty_close(conpty_handle);
      conpty_handle = null;
    }
    use_conpty = false;
  }
  term_log(&(char){'\e'}, 1);  // trigger term_log_flush();
}


void
child_update_charset(void)
{
#ifdef IUTF8
  if (pty_fd >= 0) {
    // Terminal line settings
    struct termios attr;
    tcgetattr(pty_fd, &attr);
    bool utf8 = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;
    if (utf8)
      attr.c_iflag |= IUTF8;
    else
      attr.c_iflag &= ~IUTF8;
    tcsetattr(pty_fd, TCSANOW, &attr);
  }
#endif
}

/*
   Trim environment from variables set by launchers, other terminals, 
   or other shells, in order to avoid confusing behaviour or invalid 
   information (e.g. LINES, COLUMNS).
 */
static void
trim_environment(void)
{
  void trimenv(string e) {
    if (e[strlen(e) - 1] == '_') {
#if CYGWIN_VERSION_API_MINOR >= 74
      for (int ei = 0; environ[ei]; ei++) {
        if (strncmp(e, environ[ei], strlen(e)) == 0) {
          char * ee = strchr(environ[ei], '=');
          if (ee) {
            char * ev = strndup(environ[ei], ee - environ[ei]);
            //printf("%s @%d - unsetenv %s: %s\n", e, ei, ev, environ[ei]);
            unsetenv(ev);
            free(ev);
            // recheck current position after deletion
            --ei;
          }
        }
      }
#endif
    }
    else
      unsetenv(e);
  }
  // clear startup information from desktop launchers
  trimenv("WINDOWID");
  trimenv("GIO_LAUNCHED_");
  trimenv("DESKTOP_STARTUP_ID");
  // clear optional terminal configuration indications
  trimenv("LINES");
  trimenv("COLUMNS");
  trimenv("TERMCAP");
  trimenv("COLORFGBG");
  trimenv("COLORTERM");
  trimenv("DEFAULT_COLORS");
  trimenv("WCWIDTH_CJK_LEGACY");
  // clear identification from other terminals
  trimenv("ITERM2_");
  trimenv("MC_");
  trimenv("PUTTY");
  trimenv("RXVT_");
  trimenv("URXVT_");
  trimenv("VTE_");
  trimenv("XTERM_");
  trimenv("TERM_");
  // clear indications from terminal multiplexers
  trimenv("STY");
  trimenv("WINDOW");
  trimenv("TMUX");
  trimenv("TMUX_PANE");
  trimenv("BYOBU_");
}

static bool
ispathprefix(string pref, string path)
{
  if (*pref == '/')
    pref++;
  if (*path == '/')
    path++;
  int len = strlen(pref);
  if (0 == strncmp(pref, path, len)) {
    path += len;
    if (!*path || *path == '/')
      return true;
  }
  return false;
}

// Append one argument to a command line, quoting it with rules compatible
// with CommandLineToArgvW (backslashes before a quote or the closing quote
// are doubled; quoting is triggered by embedded blanks or an empty string).
static wchar_t *
cmdline_append_arg(wchar_t *pos, const wchar_t *arg)
{
  bool quote = !*arg;
  for (const wchar_t *p = arg; *p && !quote; p++)
    quote = (*p == L' ' || *p == L'\t');
  if (quote) *pos++ = L'"';
  int bs = 0;
  const wchar_t *p = arg;
  while (*p) {
    if (*p == L'"') {
      while (bs > 0) { *pos++ = L'\\'; bs--; }
      *pos++ = L'\\';
      *pos++ = L'"';
    } else if (*p == L'\\') {
      bs++;
    } else {
      while (bs > 0) { *pos++ = L'\\'; bs--; }
      *pos++ = *p;
    }
    p++;
  }
  while (bs > 0) {
    *pos++ = L'\\';
    if (quote) *pos++ = L'\\';
    bs--;
  }
  if (quote) *pos++ = L'"';
  *pos++ = L' ';
  return pos;
}

// Whether the file name part of a path already has an extension.
static bool
wchar_name_has_ext(const wchar_t *path)
{
  const wchar_t *base = path;
  for (const wchar_t *p = path; *p; p++)
    if (*p == L'\\' || *p == L'/')
      base = p + 1;
  return !!wcschr(base, L'.');
}

void
child_create(char *argv[], struct winsize *winp)
{
  trace_dir(asform("child_create: %s", getcwd(malloc(MAX_PATH), MAX_PATH)));

  trim_environment();

  prev_winsize = *winp;

  // xterm and urxvt ignore SIGHUP, so let's do the same.
  signal(SIGHUP, SIG_IGN);

  signal(SIGINT, sigexit);
  signal(SIGTERM, sigexit);
  signal(SIGQUIT, sigexit);

  // support OSC 7 directory cloning if cloning WSL window while in rootfs
  if (support_wsl && wslname) {
    // this is done once in a new window, so don't care about memory leaks
    char * rootfs = path_win_w_to_posix(wsl_basepath);
    char * cwd = getcwd(malloc(MAX_PATH), MAX_PATH);
    if (ispathprefix(rootfs, cwd)) {
      char * dist = cs__wcstombs(wslname);
      char * wslsubdir = cwd + strlen(rootfs);
      char * wsldir = asform("//wsl$/%s%s", dist, wslsubdir);
      chdir(wsldir);
    }
  }

  // Check if ConPTY backend should be used
#ifdef MINGW_NATIVE
  // no forkpty/pty layer available; ConPTY is the only backend
  use_conpty = conpty_available();
  if (!use_conpty) {
    childerror(_("Error: ConPTY backend is not available"), true, 0, 0);
    return;
  }
#else
  use_conpty = (cfg.pty_backend != null && strcmp(cfg.pty_backend, "conpty") == 0) && conpty_available();
#endif

  if (use_conpty) {
    // === ConPTY backend path ===
    HANDLE hInput[2], hOutput[2];
    if (!CreatePipe(&hInput[0], &hInput[1], null, 0) ||
        !CreatePipe(&hOutput[0], &hOutput[1], null, 0)) {
      childerror(_("Error: Could not create ConPTY pipes"), true, 0, 0);
      return;
    }

    // Security attributes to inherit handles
    HANDLE hInputRead, hInputWrite, hOutputRead, hOutputWrite;
    if (!DuplicateHandle(GetCurrentProcess(), hInput[0], GetCurrentProcess(), &hInputRead, 0, true, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(GetCurrentProcess(), hOutput[1], GetCurrentProcess(), &hOutputWrite, 0, true, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(GetCurrentProcess(), hOutput[0], GetCurrentProcess(), &hOutputRead, 0, true, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(GetCurrentProcess(), hInput[1], GetCurrentProcess(), &hInputWrite, 0, true, DUPLICATE_SAME_ACCESS)) {
      childerror(_("Error: Could not duplicate ConPTY handles"), true, 0, 0);
      CloseHandle(hInput[0]);
      CloseHandle(hInput[1]);
      CloseHandle(hOutput[0]);
      CloseHandle(hOutput[1]);
      return;
    }
    CloseHandle(hInput[0]);
    CloseHandle(hInput[1]);
    CloseHandle(hOutput[0]);
    CloseHandle(hOutput[1]);

    // Create pseudo console (ConPTY reads keystrokes from hInputRead and
    // writes terminal output to hOutputWrite — both pipe ends it owns).
    conpty_handle = conpty_create(hInputRead, hOutputWrite);
    if (conpty_handle == null) {
      childerror(_("Error: Could not create pseudo console"), true, 0, 0);
      CloseHandle(hInputRead);
      CloseHandle(hInputWrite);
      CloseHandle(hOutputRead);
      CloseHandle(hOutputWrite);
      return;
    }
    /* Console-side pipe ends belong to ConPTY; keep the child from
       inheriting them (it receives its std handles via the pseudo console). */
    SetHandleInformation(hInputRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hOutputWrite, HANDLE_FLAG_INHERIT, 0);

    // Set process attributes for ConPTY
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(null, 1, 0, &size);
    BYTE *attr_buf = (BYTE *)malloc(size);
    if (attr_buf == null) {
      childerror(_("Error: Could not allocate process attributes"), true, 0, 0);
      conpty_close(conpty_handle);
      CloseHandle(hInputRead);
      CloseHandle(hInputWrite);
      CloseHandle(hOutputRead);
      CloseHandle(hOutputWrite);
      return;
    }

    if (!InitializeProcThreadAttributeList((LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf, 1, 0, &size)) {
      childerror(_("Error: Could not initialize process attributes"), true, 0, 0);
      free(attr_buf);
      conpty_close(conpty_handle);
      CloseHandle(hInputRead);
      CloseHandle(hInputWrite);
      CloseHandle(hOutputRead);
      CloseHandle(hOutputWrite);
      return;
    }

    if (!UpdateProcThreadAttribute((LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf, 0,
        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, conpty_handle, sizeof(HPCON), null, null)) {
      childerror(_("Error: Could not set pseudo console attribute"), true, 0, 0);
      free(attr_buf);
      conpty_close(conpty_handle);
      CloseHandle(hInputRead);
      CloseHandle(hInputWrite);
      CloseHandle(hOutputRead);
      CloseHandle(hOutputWrite);
      return;
    }

    // Build startup info.  Do NOT set STARTF_USESTDHANDLES: with
    // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE the child's std handles come from
    // the pseudo console; supplying pipe handles here would bypass it.
    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf;

    // Convert cmd and argv to wide.  The Windows CRT provides argv and the
    // derived command in the ANSI code page, independent of the terminal's
    // active code page, so decode with CP_ACP (cs__crctowcs).
    wchar_t *cmd_w = cs__crctowcs(cmd);

    int argc = 0;
    while (argv[argc]) argc++;
    wchar_t **argv_w = (wchar_t **)malloc((argc + 1) * sizeof(wchar_t *));
    if (argv_w == null) {
      childerror(_("Error: Could not allocate argument array"), true, 0, 0);
      free(attr_buf);
      free(cmd_w);
      conpty_close(conpty_handle);
      CloseHandle(hInputRead);
      CloseHandle(hInputWrite);
      CloseHandle(hOutputRead);
      CloseHandle(hOutputWrite);
      return;
    }
    for (int i = 0; i < argc; i++) {
      argv_w[i] = cs__crctowcs(argv[i]);
      if (argv_w[i] == null) {
        childerror(_("Error: Could not convert argument"), true, 0, 0);
        for (int j = 0; j < i; j++) free(argv_w[j]);
        free(argv_w);
        free(attr_buf);
        free(cmd_w);
        conpty_close(conpty_handle);
        CloseHandle(hInputRead);
        CloseHandle(hInputWrite);
        CloseHandle(hOutputRead);
        CloseHandle(hOutputWrite);
        return;
      }
    }
    argv_w[argc] = null;

    // Build the command line from all of argv with proper quoting; the
    // program to execute goes to lpApplicationName below, so an argv[0]
    // such as "-sh" for a login shell cannot break process creation.
    size_t cl_chars = wcslen(cmd_w) + 2;
    for (int i = 0; i < argc; i++)
      cl_chars += wcslen(argv_w[i]) * 2 + 4;
    wchar_t *cmdline = (wchar_t *)malloc(cl_chars * sizeof(wchar_t));
    if (cmdline == null) {
      childerror(_("Error: Could not allocate command line"), true, 0, 0);
      for (int i = 0; i < argc; i++) free(argv_w[i]);
      free(argv_w);
      free(attr_buf);
      free(cmd_w);
      conpty_close(conpty_handle);
      CloseHandle(hInputRead);
      CloseHandle(hInputWrite);
      CloseHandle(hOutputRead);
      CloseHandle(hOutputWrite);
      return;
    }
    wchar_t *pos = cmdline;
    if (argc > 0) {
      for (int i = 0; i < argc; i++)
        pos = cmdline_append_arg(pos, argv_w[i]);
    } else {
      pos = cmdline_append_arg(pos, cmd_w);
    }
    *pos = '\0';

    // Create process.  bInheritHandles is false: the child must not inherit
    // our pipe ends.
    // Note that Windows still copies our own standard handles (taken from our
    // PEB, not the handle table) into the child, irrespective of
    // bInheritHandles.  Those would shadow the handles supplied by the pseudo
    // console, so the child would write its stdio to imintty's own stdout and
    // stderr instead of to the pty (and would then not be a tty either).
    // Temporarily clear them so that the pseudo console handles are used.
    HANDLE saved_std[3];
    saved_std[0] = GetStdHandle(STD_INPUT_HANDLE);
    saved_std[1] = GetStdHandle(STD_OUTPUT_HANDLE);
    saved_std[2] = GetStdHandle(STD_ERROR_HANDLE);
    SetStdHandle(STD_INPUT_HANDLE, null);
    SetStdHandle(STD_OUTPUT_HANDLE, null);
    SetStdHandle(STD_ERROR_HANDLE, null);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    DWORD werr = 0;
    bool created = CreateProcessW(cmd_w, cmdline, null, null, false,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        null, null, (LPSTARTUPINFOW)&si, &pi);
    if (!created)
      werr = GetLastError();
    // CreateProcessW does not append ".exe" when lpApplicationName is given
    // (MSYS2 argv conversion yields extension-less paths like /x/bash);
    // retry with the extension if that file exists.
    if (!created && (werr == ERROR_FILE_NOT_FOUND || werr == ERROR_PATH_NOT_FOUND)
        && !wchar_name_has_ext(cmd_w)) {
      size_t xl = wcslen(cmd_w);
      wchar_t *cmd_x = (wchar_t *)malloc((xl + 5) * sizeof(wchar_t));
      if (cmd_x) {
        wcscpy(cmd_x, cmd_w);
        wcscat(cmd_x, L".exe");
        DWORD attr = GetFileAttributesW(cmd_x);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
          created = CreateProcessW(cmd_x, cmdline, null, null, false,
              EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
              null, null, (LPSTARTUPINFOW)&si, &pi);
          if (!created)
            werr = GetLastError();
        }
        free(cmd_x);
      }
    }
    // Restore our own standard handles (imintty may still log to stderr).
    SetStdHandle(STD_INPUT_HANDLE, saved_std[0]);
    SetStdHandle(STD_OUTPUT_HANDLE, saved_std[1]);
    SetStdHandle(STD_ERROR_HANDLE, saved_std[2]);
    if (!created) {
      char *cmd8 = cs__wcstoutf(cmd_w);
      char *cl8 = cs__wcstoutf(cmdline);
      free(cmd8);
      free(cl8);
      childerror(_("Error: Could not create ConPTY child process"), true, 0, 0);
      for (int i = 0; i < argc; i++) free(argv_w[i]);
      free(argv_w);
      free(attr_buf);
      free(cmdline);
      free(cmd_w);
      conpty_close(conpty_handle);
      CloseHandle(hInputRead);
      CloseHandle(hInputWrite);
      CloseHandle(hOutputRead);
      CloseHandle(hOutputWrite);
      return;
    }

    // Cleanup ConPTY-specific resources
    free(attr_buf);
    for (int i = 0; i < argc; i++) free(argv_w[i]);
    free(argv_w);
    free(cmdline);
    free(cmd_w);

    // Store state
    pid = pi.dwProcessId;
    conpty_input = hInputWrite;
    conpty_output = hOutputRead;
    pty_fd = -1;  // Mark non-pty mode

#ifdef MINGW_NATIVE
  }
#else
  } else {
    // === MSYS/forkpty backend path (original) ===
    // Create the child process and pseudo terminal.
    pid = forkpty(&pty_fd, 0, 0, winp);
    if (pid < 0) {
      bool rebase_prompt = (errno == EAGAIN);
      //ENOENT  There are no available terminals.
      //EAGAIN  Cannot allocate sufficient memory to allocate a task structure.
      //EAGAIN  Not possible to create a new process; RLIMIT_NPROC limit.
      //ENOMEM  Memory is tight.
      childerror(_("Error: Could not fork child process"), true, errno, pid);
      if (rebase_prompt)
        childerror(_("DLL rebasing may be required; see 'rebaseall / rebase --help'"), false, 0, 0);

      pid = 0;

      term_hide_cursor();
    }
    else if (!pid) { // Child process.
#if CYGWIN_VERSION_DLL_MAJOR < 1007
      // Some native console programs require a console to be attached to the
      // process, otherwise they pop one up themselves, which is rather annoying.
      // Cygwin's exec function from 1.5 onwards automatically allocates a console
      // on an invisible window station if necessary. Unfortunately that trick no
      // longer works on Windows 7, which is why Cygwin 1.7 contains a new hack
      // for creating the invisible console.
      // On Cygwin versions before 1.5 and on Cygwin 1.5 running on Windows 7,
      // we need to create the invisible console ourselves. The hack here is not
      // as clever as Cygwin's, with the console briefly flashing up on startup,
      // but it'll do.
#if CYGWIN_VERSION_DLL_MAJOR == 1005
      DWORD win_version = GetVersion();
      win_version = ((win_version & 0xff) << 8) | ((win_version >> 8) & 0xff);
      if (win_version >= 0x0601)  // Windows 7 is NT 6.1.
#endif
        if (AllocConsole()) {
          HMODULE kernel = GetModuleHandleA("kernel32");
          HWND (WINAPI *pGetConsoleWindow)(void) =
            (void *)GetProcAddress(kernel, "GetConsoleWindow");
          ShowWindowAsync(pGetConsoleWindow(), SW_HIDE);
        }
#endif

      // Reset signals
      signal(SIGHUP, SIG_DFL);
      signal(SIGINT, SIG_DFL);
      signal(SIGQUIT, SIG_DFL);
      signal(SIGTERM, SIG_DFL);
      signal(SIGCHLD, SIG_DFL);

      // Mimick login's behavior by disabling the job control signals
      signal(SIGTSTP, SIG_IGN);
      signal(SIGTTIN, SIG_IGN);
      signal(SIGTTOU, SIG_IGN);

      setenv("TERM", cfg.term, true);
      // unreliable info about terminal application (#881)
      setenv("TERM_PROGRAM", APPNAME, true);
      setenv("TERM_PROGRAM_VERSION", VERSION, true);

      // If option Locale is used, set locale variables?
      // https://github.com/imintty/imintty/issues/116#issuecomment-108888265
      // Variables are now set in update_locale() which sets one of 
      // LC_ALL or LC_CTYPE depending on previous setting of 
      // LC_ALL or LC_CTYPE or LANG, stripping @cjk modifiers for WSL.
      if (cfg.old_locale) {
        //string lang = cs_lang();
        string lang = cs_lang() ? cs_get_locale() : 0;
        if (lang) {
          unsetenv("LC_ALL");
          unsetenv("LC_COLLATE");
          unsetenv("LC_CTYPE");
          unsetenv("LC_MONETARY");
          unsetenv("LC_NUMERIC");
          unsetenv("LC_TIME");
          unsetenv("LC_MESSAGES");
          setenv("LANG", lang, true);
        }
      }

      // Terminal line settings
      struct termios attr;
      tcgetattr(0, &attr);
      attr.c_cc[VERASE] = cfg.backspace_sends_bs ? CTRL('H') : CDEL;
      attr.c_iflag |= IXANY | IMAXBEL;
#ifdef IUTF8
      bool utf8 = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;
      if (utf8)
        attr.c_iflag |= IUTF8;
      else
        attr.c_iflag &= ~IUTF8;
#endif
      attr.c_lflag |= ECHOE | ECHOK | ECHOCTL | ECHOKE;
      tcsetattr(0, TCSANOW, &attr);

      // Invoke command
      execvp(cmd, argv);

      // If we get here, exec failed.
      fprintf(stderr, "\033]701;C.UTF-8\007");
      fprintf(stderr, "\033[30;41m\033[K");
      //__ %1$s: client command (e.g. shell) to be run; %2$s: error message
      fprintf(stderr, _("Failed to run '%s': %s"), cmd, strerror(errno));
      fprintf(stderr, "\r\n");
      fflush(stderr);

#if CYGWIN_VERSION_DLL_MAJOR < 1005
      // Before Cygwin 1.5, the message above doesn't appear if we exit
      // immediately. So have a little nap first.
      usleep(200000);
#endif

      exit(mexit);
    }
    else { // Parent process.
      if (report_child_pid) {
        printf("%d\n", pid);
        fflush(stdout);
      }
      if (report_child_tty) {
        printf("%s\n", ptsname(pty_fd));
        fflush(stdout);
      }

#ifdef __midipix__
      // This corrupts CR in cygwin
      struct termios attr;
      tcgetattr(pty_fd, &attr);
      cfmakeraw(&attr);
      tcsetattr(pty_fd, TCSANOW, &attr);
#endif

      fcntl(pty_fd, F_SETFL, O_NONBLOCK);

      //child_update_charset();  // could do it here or as above

      if (cfg.create_utmp) {
        char *dev = ptsname(pty_fd);
        if (dev) {
          struct utmp ut;
          memset(&ut, 0, sizeof ut);

          if (!strncmp(dev, "/dev/", 5))
            dev += 5;
          strlcpy(ut.ut_line, dev, sizeof ut.ut_line);

          if (dev[1] == 't' && dev[2] == 'y')
            dev += 3;
          else if (!strncmp(dev, "pts/", 4))
            dev += 4;
          //strncpy(ut.ut_id, dev, sizeof ut.ut_id);
          for (uint i = 0; i < sizeof ut.ut_id && *dev; i++)
            ut.ut_id[i] = *dev++;

          ut.ut_type = USER_PROCESS;
          ut.ut_pid = pid;
          ut.ut_time = time(0);
          strlcpy(ut.ut_user, getlogin() ?: "?", sizeof ut.ut_user);
          gethostname(ut.ut_host, sizeof ut.ut_host);
          login(&ut);
        }
      }
    }
  }
#endif /* !MINGW_NATIVE */

#ifdef MINGW_NATIVE
  win_fd = -1;  // no /dev/windows multiplexer; child_proc waits on messages
#else
  win_fd = open("/dev/windows", O_RDONLY);
#endif

  if (cfg.logging) {
    // option Logging=yes => initially open log file if configured
    open_logfile(false);
  }
}

char *
child_tty(void)
{
  if (use_conpty)
    return "conpty";
#ifdef MINGW_NATIVE
  return null;
#else
  return ptsname(pty_fd);
#endif
}

uchar *
child_termios_chars(void)
{
  if (use_conpty) {
    // no tty available: fall back to conventional default special characters
    static uchar def_cc[NCCS];
    static bool def_cc_init = false;
    if (!def_cc_init) {
      memset(def_cc, 0, sizeof def_cc);
      def_cc[VINTR] = 'C' & 0x1F;    // ^C
      def_cc[VQUIT] = '\\' & 0x1F;   // Ctrl-backslash
      def_cc[VSUSP] = 'Z' & 0x1F;    // ^Z
      def_cc[VSWTC] = 0;
      def_cc[VERASE] = CERASE;
      def_cc[VKILL] = 'U' & 0x1F;    // ^U
      def_cc_init = true;
    }
    return def_cc;
  }
#ifdef MINGW_NATIVE
  return null;
#else
  {
    static struct termios attr;
    tcgetattr(pty_fd, &attr);
    return attr.c_cc;
  }
#endif
}

#define patch_319

#ifdef debug_pty
static void
trace_line(char * tag, int n, const char * s, int len)
{
  printf("%s %d", tag, n);
  for (int i = 0; i < len; i++)
    printf(" %02X", (uint)s[i]);
  printf("\n");
}
#else
#define trace_line(tag, n, s, len)	
#endif

void
child_proc(void)
{
  if (term.no_scroll)
    return;

  for (;;) {
    if (term.paste_buffer)
      term_send_paste();

    struct timeval timeout = {0, 100000}, *timeout_p = 0;
    fd_set fds;
    FD_ZERO(&fds);
#ifndef MINGW_NATIVE
    FD_SET(win_fd, &fds);
    if (pty_fd >= 0)
      FD_SET(pty_fd, &fds);
#endif
#ifndef patch_319
    else
#endif
    if (pid) {
      int status;
      if (waitpid(pid, &status, WNOHANG) == pid) {
        pid = 0;

        // Decide whether we want to exit now or later
        if (killed || cfg.hold == HOLD_NEVER)
          exit_imintty();
        else if (cfg.hold == HOLD_START) {
          if (WIFSIGNALED(status) || WEXITSTATUS(status) != mexit)
            exit_imintty();
        }
        else if (cfg.hold == HOLD_ERROR) {
          if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == 0)
              exit_imintty();
          }
          else {
            const int error_sigs =
              1 << SIGILL | 1 << SIGTRAP | 1 << SIGABRT | 1 << SIGFPE |
              1 << SIGBUS | 1 << SIGSEGV | 1 << SIGPIPE | 1 << SIGSYS;
            if (!(error_sigs & 1 << WTERMSIG(status)))
              exit_imintty();
          }
        }

        char *s = 0;
        bool err = true;
        if (WIFEXITED(status)) {
          int code = WEXITSTATUS(status);
          if (code == 0)
            err = false;
          if ((code || cfg.exit_write) /*&& cfg.hold != HOLD_START*/)
            //__ %1$s: client command (e.g. shell) terminated, %2$i: exit code
            asprintf(&s, _("%s: Exit %i"), cmd, code);
        }
        else if (WIFSIGNALED(status))
          asprintf(&s, "%s: %s", cmd, strsignal(WTERMSIG(status)));

        if (!s && cfg.exit_write) {
          //__ default inline notification if ExitWrite=yes
          s = _("TERMINATED");
        }
        if (s) {
          char * wsl_pre = "\0337\033[H\033[L";
          char * wsl_post = "\0338\033[B";
          if (err && support_wsl)
            term_write(wsl_pre, strlen(wsl_pre));
          childerror(s, false, 0, err ? 41 : 42);
          if (err && support_wsl)
            term_write(wsl_post, strlen(wsl_post));
        }

        if (cfg.exit_title && *cfg.exit_title)
          win_prefix_title(cfg.exit_title);
      }
#ifdef patch_319
      if (pid != 0 && pty_fd < 0) // Pty gone, but process still there: keep checking
#else
      else // Pty gone, but process still there: keep checking
#endif
        timeout_p = &timeout;
    }

#ifdef exit_WSL_after_closed_here
    if (support_wsl && killed)
      // force-close, as wsl.exe does not response to kill() in Windows 10
      exit_imintty();
#endif

    bool ready = false;
#ifdef MINGW_NATIVE
    // P8: wait for ConPTY output data, pipe EOF, or a pending window message.
    if (conpty_output) {
      DWORD avail = 0;
      if (PeekNamedPipe(conpty_output, null, 0, null, &avail, null)) {
        if (avail > 0)
          ready = true;
      }
      else
        ready = true;  // EOF: pipe closed, let the handler below clean up
    }
    if (!ready) {
      DWORD wt = INFINITE;
      if (timeout_p)
        wt = (DWORD)(timeout_p->tv_sec * 1000 + timeout_p->tv_usec / 1000);
      DWORD r = MsgWaitForMultipleObjects(
                  conpty_output ? 1 : 0, &conpty_output, FALSE, wt, QS_ALLINPUT);
      if (conpty_output && r == WAIT_OBJECT_0)
        ready = true;
      else if (r == WAIT_OBJECT_0 + (conpty_output ? 1 : 0))
        return;  // window message pending: let the main message loop dispatch it
      // WAIT_TIMEOUT: loop again (e.g. to poll for child process exit)
    }
#else
    ready = select(win_fd + 1, &fds, 0, 0, timeout_p) > 0;
#endif
    if (ready) {
      if (use_conpty) {
        // ConPTY backend: use ReadFile on conpty_output handle
        static char buf[4096];
        DWORD bytes_read = 0;
        if (conpty_output) {
          // Check if process has exited (pipe closed = EOF)
          if (!PeekNamedPipe(conpty_output, null, 0, null, &bytes_read, null)) {
            // Pipe closed - process exited
            pid = 0;
            if (conpty_handle) {
              conpty_close(conpty_handle);
              conpty_handle = null;
            }
            if (conpty_input) {
              CloseHandle(conpty_input);
              conpty_input = null;
            }
            if (conpty_output) {
              CloseHandle(conpty_output);
              conpty_output = null;
            }
            if (killed || cfg.hold == HOLD_NEVER)
              exit_imintty();
            term_hide_cursor();
          }
          else if (bytes_read > 0) {
            if (ReadFile(conpty_output, buf, sizeof buf, &bytes_read, null)) {
              if (bytes_read > 0) {
                term_write(buf, (uint)bytes_read);
                trace_line("twrt", (int)bytes_read, buf, (int)bytes_read);
                DBGx(LOG_FULL, "[t%d] tty< %.*s\n", get_tick_count(),
                     (int)(bytes_read < 96 ? bytes_read : 96), buf);

                // accelerate keyboard echo if (unechoed) keyboard input is pending
                if (kb_input) {
                  kb_input = false;
                  if (cfg.display_speedup)
                    // undocumented safeguard in case something goes wrong here
                    win_update_now();
                }
                term_log(buf, (uint)bytes_read);
              }
            }
          }
        }
      }
      else if (pty_fd >= 0 && FD_ISSET(pty_fd, &fds)) {
        // Pty devices on old Cygwin versions (pre 1005) deliver only 4 bytes
        // at a time, and newer ones or MSYS2 deliver up to 256 at a time.
        // so call read() repeatedly until we have a worthwhile haul.
        // this avoids most partial updates, results in less flickering/tearing.
        static char buf[4096];
        uint len = 0;
#if CYGWIN_VERSION_API_MINOR >= 74
        if (term.baud > 0) {
          uint cps = term.baud / 10; // 1 start bit, 8 data bits, 1 stop bit
          uint nspc = 2000000000 / cps;

          static ulong prevtime = 0;
          static ulong exceeded = 0;
          static ulong granularity = 0;
          struct timespec tim;
          if (!granularity) {
            clock_getres(CLOCK_MONOTONIC, &tim); // cygwin granularity: 539ns
            granularity = tim.tv_nsec;
          }
          clock_gettime(CLOCK_MONOTONIC, &tim);
          ulong now = tim.tv_sec * (long)1000000000 + tim.tv_nsec;
          //printf("baud %d ns/char %d prev %ld now %ld delta\n", term.baud, nspc, prevtime, now);
          if (now < prevtime + nspc) {
            ulong delay = prevtime ? prevtime + nspc - now : 0;
            if (delay < exceeded)
              exceeded -= delay;
            else {
              tim.tv_sec = delay / 1000000000;
              tim.tv_nsec = delay % 1000000000;
              clock_nanosleep(CLOCK_MONOTONIC, 0, &tim, 0);
              clock_gettime(CLOCK_MONOTONIC, &tim);
              ulong then = tim.tv_sec * (long)1000000000 + tim.tv_nsec;
              //printf("nsleep %ld -> %ld\n", delay, then - now);
              if (then - now > delay)
                exceeded = then - now - delay;
              now = then;
            }
          }
          prevtime = now;

          int ret = read(pty_fd, buf, 1);
          if (ret > 0)
            len = ret;
        }
        else
#endif
#if defined(collect_pty_buffer) || CYGWIN_VERSION_DLL_MAJOR < 1005
        do
#endif
        {
          int ret = read(pty_fd, buf + len, sizeof buf - len);
          //printf("%d+%d ", len, ret);
          trace_line("read", ret, buf + len, ret);
          //if (kb_trace) printf("[%lu] read %d\n", mtime(), ret);

          if (ret > 0)
            len += ret;
          else
            break;

          // The read loop buffer filling was once introduced to speed up
          // but its implied usage of pty interferes with the WSL gateway
          // wsl.exe and its conpty layer in some inexplicable way (#1332).
          // (This did not happen before cygwin 3.3.5.)
          // It is particularly completely obscure how the corrupted 
          // display described there is triggered by entering ^O.
          // So we disable the loop. It is apparently not needed anymore.
          // (It could be disabled only while wsl.exe is running which
          // could be detected by catching the DECSET 9001 sequence.)
        }
#if defined(collect_pty_buffer) || CYGWIN_VERSION_DLL_MAJOR < 1005
          while (len < sizeof buf);
#endif
        //printf("read %d\n", len);

        if (len > 0) {
          term_write(buf, len);
          trace_line("twrt", len, buf, len);

          // accelerate keyboard echo if (unechoed) keyboard input is pending
          if (kb_input) {
            kb_input = false;
            if (cfg.display_speedup)
              // undocumented safeguard in case something goes wrong here
              win_update_now();
          }
          term_log(buf, len);
        }
        else {
          pty_fd = -1;
          term_hide_cursor();
        }
      }
#ifndef MINGW_NATIVE
      if (FD_ISSET(win_fd, &fds))
        return;
#endif
    }
  }
}

void
child_kill(bool point_blank)
{
  if (use_conpty) {
    // ConPTY: terminate Windows process
    if (pid) {
      HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
      if (hProcess) {
        TerminateProcess(hProcess, point_blank ? 1 : 0);
        CloseHandle(hProcess);
      }
    }
    if (point_blank || !pid)
      exit_imintty();
    killed = true;
    return;
  }
  if (!pid ||
      kill(-pid, point_blank ? SIGKILL : SIGHUP) < 0 ||
      point_blank)
    exit_imintty();

  if (support_wsl) {
    //if (killed) // limit to double-close? - rather not

    // force-close, as wsl.exe does not response to kill() in Windows 10
    exit_imintty();
  }

  killed = true;
}

bool
child_is_alive(void)
{
  return pid;
}

bool
child_is_parent(void)
{
  if (!pid)
    return false;
#if defined(__MINGW32__) || defined(__MINGW64__)
  // P8: check via Toolhelp whether any process has our child as parent
  bool res = false;
  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnap == INVALID_HANDLE_VALUE)
    return false;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(hSnap, &pe)) {
    do {
      if (pe.th32ParentProcessID == (DWORD)pid && pe.th32ProcessID != (DWORD)pid) {
        res = true;
        break;
      }
    } while (Process32NextW(hSnap, &pe));
  }
  CloseHandle(hSnap);
  return res;
#else
  DIR * d = opendir("/proc");
  if (!d)
    return false;
  bool res = false;
  struct dirent * e;
  while ((e = readdir(d))) {
    char * pn = e->d_name;
    if (isdigit((uchar)*pn) && strlen(pn) <= 6) {
      char * fn = asform("/proc/%s/ppid", pn);
      FILE * f = fopen(fn, "r");
      free(fn);
      if (!f)
        continue;
      pid_t ppid = 0;
      fscanf(f, "%u", &ppid);
      fclose(f);
      if (ppid == pid) {
        res = true;
        break;
      }
    }
  }
  closedir(d);
  return res;
#endif
}

static struct procinfo {
  int pid;
  int ppid;
  int winpid;
  char * cmdline;
} * ttyprocs = 0;
static uint nttyprocs = 0;

char *
procres(int pid, char * res)
{
#if defined(__MINGW32__) || defined(__MINGW64__)
  /* P8: Toolhelp32 — read exe name (cmdline not available without NT APIs) */
  (void)res;
  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(hSnap, &pe)) {
    do {
      if (pe.th32ProcessID == (DWORD)pid) {
        char *out = cs__wcstoutf(pe.szExeFile);
        CloseHandle(hSnap);
        return out;
      }
    } while (Process32NextW(hSnap, &pe));
  }
  CloseHandle(hSnap);
  return 0;
#else
  char fbuf[99];
  char * fn = asform("/proc/%d/%s", pid, res);
  int fd = open(fn, O_BINARY | O_RDONLY);
  free(fn);
  if (fd < 0)
    return 0;
  int n = read(fd, fbuf, sizeof fbuf - 1);
  close(fd);
  for (int i = 0; i < n - 1; i++)
    if (!fbuf[i])
      fbuf[i] = ' ';
  fbuf[n] = 0;
  char * nl = strchr(fbuf, '\n');
  if (nl)
    *nl = 0;
  return strdup(fbuf);
#endif
}

#ifndef MINGW_NATIVE
static int
procresi(int pid, char * res)
{
  char * si = procres(pid, res);
  int i = atoi(si);
  free(si);
  return i;
}
#endif

#ifndef HAS_LOCALES
#define wcwidth xcwidth
#else
#if CYGWIN_VERSION_API_MINOR < 74
#define wcwidth xcwidth
#endif
#endif

wchar *
grandchild_process_list(void)
{
  if (!pid)
    return 0;
#if defined(__MINGW32__) || defined(__MINGW64__)
  /* P8: Toolhelp32-based process listing for MinGW.
   * Build a single pid→ppid snapshot, then BFS from our child pid DOWN
   * through a children-list to collect exactly its descendant subtree. */
  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  /* Process32First MUST be consumed (previous code skipped the first entry). */
  if (!Process32FirstW(hSnap, &pe)) { CloseHandle(hSnap); return 0; }
  /* children list + exe name (max 65536 PIDs per Windows). */
  #define PID_MAP_SIZE 65536
  static int   child_count[PID_MAP_SIZE];
  static DWORD children[PID_MAP_SIZE][128];  /* up to 128 children per proc */
  static char *exe_name[PID_MAP_SIZE];       /* malloc'd, freed later */
  static bool  map_ready = false;
  if (!map_ready) {
    for (int i = 0; i < PID_MAP_SIZE; i++) {
      child_count[i] = 0;
      exe_name[i] = 0;
    }
    map_ready = true;
  }
  do {
    DWORD pid_val  = pe.th32ProcessID;
    DWORD ppid_val = pe.th32ParentProcessID;
    if (pid_val < PID_MAP_SIZE) {
      /* Store exe name (wide→UTF-8 for later narrow display). */
      exe_name[pid_val] = cs__wcstoutf(pe.szExeFile);
      if (ppid_val < PID_MAP_SIZE) {
        int *cnt = &child_count[ppid_val];
        if (*cnt < 128)
          children[ppid_val][(*cnt)++] = pid_val;
      }
    }
  } while (Process32NextW(hSnap, &pe));
  CloseHandle(hSnap);

  /* BFS from `pid` — collect only true descendants, skip `pid` itself. */
  int queue[8192];
  int qhead = 0, qtail = 0;
  bool visited[PID_MAP_SIZE];
  for (int i = 0; i < PID_MAP_SIZE; i++) visited[i] = false;
  queue[qtail++] = pid;
  visited[pid] = true;
  while (qhead < qtail) {
    int cur = queue[qhead++];
    /* Enumerate direct children of `cur` (true descendants). */
    if (cur >= 0 && cur < PID_MAP_SIZE) {
      int cc = child_count[cur];
      for (int ci = 0; ci < cc; ci++) {
        DWORD child = children[cur][ci];
        if (child < PID_MAP_SIZE && !visited[(int)child]) {
          visited[(int)child] = true;
          queue[qtail++] = (int)child;
          ttyprocs = renewn(ttyprocs, nttyprocs + 1);
          ttyprocs[nttyprocs].pid     = (int)child;
          ttyprocs[nttyprocs].ppid    = cur;
          ttyprocs[nttyprocs].winpid  = (int)child;
          ttyprocs[nttyprocs].cmdline = exe_name[child];
          nttyprocs++;
        }
      }
    }
  }
  /* Free exe names allocated during snapshot. */
  for (int i = 0; i < PID_MAP_SIZE; i++) {
    if (exe_name[i]) { free(exe_name[i]); exe_name[i] = 0; }
  }
#else
  DIR * d = opendir("/proc");
  if (!d)
    return 0;
  char * tty = child_tty();
  struct dirent * e;
  while ((e = readdir(d))) {
    char * pn = e->d_name;
    int thispid = atoi(pn);
    if (thispid && thispid != pid) {
      char * ctty = procres(thispid, "ctty");
      if (ctty) {
        if (0 == strcmp(ctty, tty)) {
          int ppid = procresi(thispid, "ppid");
          int winpid = procresi(thispid, "winpid");
          ttyprocs = renewn(ttyprocs, nttyprocs + 1);
          ttyprocs[nttyprocs].pid = thispid;
          ttyprocs[nttyprocs].ppid = ppid;
          ttyprocs[nttyprocs].winpid = winpid;
          char * cmd = procres(thispid, "cmdline");
          ttyprocs[nttyprocs].cmdline = cmd;
          nttyprocs++;
        }
        free(ctty);
      }
    }
  }
  closedir(d);
#endif

  DWORD win_version = GetVersion();
  win_version = ((win_version & 0xff) << 8) | ((win_version >> 8) & 0xff);

  wchar * res = 0;
  for (uint i = 0; i < nttyprocs; i++) {
    char * proc = newn(char, 50 + strlen(ttyprocs[i].cmdline));
    sprintf(proc, " %5u %5u %s", ttyprocs[i].winpid, ttyprocs[i].pid, ttyprocs[i].cmdline);
    free(ttyprocs[i].cmdline);
#ifdef MINGW_NATIVE
    /* MINGW: cmdline comes from cs__wcstoutf (UTF-8) */
    wchar * procw = cs__utftowcs(proc);
#else
    wchar * procw = cs__mbstowcs(proc);
#endif
    free(proc);
    if (win_version >= 0x0601)
      for (int i = 0; i < 13; i++)
        if (procw[i] == ' ')
          procw[i] = 0x2007;  // FIGURE SPACE
    int wid = min(wcslen(procw), 40);
    for (int i = 13; i < wid; i++)
      if (((cfg.charwidth % 10) ? xcwidth(procw[i]) : wcwidth(procw[i])) == 2)
        wid--;
    procw[wid] = 0;

    if (win_version >= 0x0601) {
      if (!res)
        res = wcsdup(W("╎ WPID   PID  COMMAND\n"));
      res = renewn(res, wcslen(res) + wcslen(procw) + 3);
      wcscat(res, W("╎"));
    }
    else {
      if (!res)
        res = wcsdup(W("| WPID   PID  COMMAND\n"));
      res = renewn(res, wcslen(res) + wcslen(procw) + 3);
      wcscat(res, W("|"));
    }

    wcscat(res, procw);
    wcscat(res, W("\n"));
    free(procw);
  }
  if (ttyprocs) {
    nttyprocs = 0;
    free(ttyprocs);
    ttyprocs = 0;
  }
  return res;
}

void
child_write(const char *buf, uint len)
{
  if (use_conpty) {
    if (conpty_input) {
      DWORD written = 0;
      WriteFile(conpty_input, buf, (DWORD)len, &written, null);
      trace_line("cwrt", (int)written, buf, (int)len);
    }
  }
  else if (pty_fd >= 0) {
#ifdef debug_pty
    int n =
#endif
    write(pty_fd, buf, len);
    trace_line("cwrt", n, buf, len);
  }
}

/*
  Simulate a BREAK event.
 */
void
child_break()
{
  int gid = tcgetpgrp(pty_fd);
  if (gid > 1) {
    struct termios attr;
    tcgetattr(pty_fd, &attr);
    if ((attr.c_iflag & (IGNBRK | BRKINT)) == BRKINT) {
      kill(gid, SIGINT);
    }
  }
}

/*
  Send an INTR char.
 */
void
child_intr()
{
  char * c_cc = (char *)child_termios_chars();
  child_write(&c_cc[VINTR], 1);
}

void
child_printf(const char *fmt, ...)
{
  if (pty_fd >= 0) {
    va_list va;
    va_start(va, fmt);
    char *s;
    int len = vasprintf(&s, fmt, va);
    va_end(va);
    if (len >= 0) {
#ifdef debug_pty
      int n =
#endif
      write(pty_fd, s, len);
      trace_line("tprt", n, s, len);
    }
    free(s);
  }
}

void
child_send(const char *buf, uint len)
{
  term_reset_screen();
  if (term.echoing)
    term_write(buf, len);
  child_write(buf, len);
}

void
child_sendw(const wchar *ws, uint wlen)
{
  char s[wlen * cs_cur_max];
  int len = cs_wcntombn(s, ws, sizeof s, wlen);
  if (len > 0)
    child_send(s, len);
}

void
child_resize(struct winsize *winp)
{
  if (use_conpty) {
    if (memcmp(&prev_winsize, winp, sizeof(struct winsize)) != 0) {
      prev_winsize = *winp;
      if (conpty_handle)
        conpty_resize(conpty_handle, winp->ws_col, winp->ws_row);
    }
  }
  else if (pty_fd >= 0 && memcmp(&prev_winsize, winp, sizeof(struct winsize)) != 0) {
    prev_winsize = *winp;
    ioctl(pty_fd, TIOCSWINSZ, winp);
  }
}

static int
foreground_pid()
{
  int fg_pid = (pty_fd >= 0) ? tcgetpgrp(pty_fd) : 0;
  if (fg_pid <= 0 && use_conpty)
    fg_pid = pid;
  if (fg_pid <= 0)
    // fallback to child process
    fg_pid = pid;
  return fg_pid;
}

static char *
get_foreground_cwd()
{
  // for WSL, do not check foreground process; hope start dir is good
  if (support_wsl) {
    char cwd[MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)))
      return strdup(cwd);
    else
      return 0;
  }

#if CYGWIN_VERSION_DLL_MAJOR >= 1005
  int fg_pid = foreground_pid();
  if (fg_pid > 0) {
    char proc_cwd[32];
    sprintf(proc_cwd, "/proc/%u/cwd", fg_pid);
    char * rp = realpath(proc_cwd, 0);
    //printf("/proc/%u/cwd: %s\n", fg_pid, rp);

    // we have kind of a race condition here as the foreground process 
    // just determined may already have terminated when we try to 
    // retrieve its working directory - 
    // this may particularly happen if dynamic setting of window icon 
    // or window background is done by a service tool;
    // so we do a single retry in this case, likely getting the 
    // working directory of the shell or other parent process
    if (!rp) {
      // ... however, even though the failed realpath() above 
      // would suggest the (previous) foreground process to be terminated, 
      // occasionally the subsequent foreground_pid returned the same 
      // process id again (mysterious);
      // so we add a tiny delay here which seems to mitigate the glitch
      usleep(2000);
      fg_pid = foreground_pid();

      if (fg_pid > 0) {
        sprintf(proc_cwd, "/proc/%u/cwd", fg_pid);
        rp = realpath(proc_cwd, 0);
      }
    }
    return rp;
  }
#endif
  return 0;
}

char *
foreground_cwd()
{
#ifdef consider_osc7
  // if working dir is communicated interactively, use it
  // - drop this generic handling here as it would also affect 
  //   relative pathname resolution outside OSC 7 usage
  if (child_dir && *child_dir)
    return strdup(child_dir);
#endif
  return get_foreground_cwd();
}

char *
foreground_prog()
{
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
  int fg_pid = foreground_pid();
  if (fg_pid > 0) {
    char exename[32];
    sprintf(exename, "/proc/%u/exename", fg_pid);
    FILE * enf = fopen(exename, "r");
    if (enf) {
      char exepath[MAX_PATH + 1];
      fgets(exepath, sizeof exepath, enf);
      fclose(enf);
      // get basename of program path
      char * exebase = strrchr(exepath, '/');
      if (exebase)
        exebase++;
      else
        exebase = exepath;
      return strdup(exebase);
    }
  }
#endif
  return 0;
}

void
user_command(wstring commands, int n)
{
  if (*commands) {
    char * cmds = cs__wcstombs(commands);
    char * cmdp = cmds;
    char sepch = ';';
    if ((uchar)*cmdp <= (uchar)' ')
      sepch = *cmdp++;

    char * progp;
    while (n >= 0 && (progp = strchr(cmdp, ':'))) {
      progp++;
      char * sepp = strchr(progp, sepch);
      if (sepp)
        *sepp = '\0';

      if (n == 0) {
        int fgpid = foreground_pid();
        if (fgpid) {
          char * _fgpid = 0;
          asprintf(&_fgpid, "%d", fgpid);
          if (_fgpid) {
            setenv("IMINTTY_PID", _fgpid, true);
            free(_fgpid);
          }
        }
        char * fgp = foreground_prog();
        if (fgp) {
          setenv("IMINTTY_PROG", fgp, true);
          free(fgp);
        }
        char * fgd = foreground_cwd();
        if (fgd) {
          setenv("IMINTTY_CWD", fgd, true);
          free(fgd);
        }
        term_cmd(progp);
        unsetenv("IMINTTY_CWD");
        unsetenv("IMINTTY_PROG");
        unsetenv("IMINTTY_PID");
        break;
      }
      n--;

      if (sepp) {
        cmdp = sepp + 1;
        // check for multi-line separation
        if (*cmdp == '\\' && cmdp[1] == '\n') {
          cmdp += 2;
          while (iswspace(*cmdp))
            cmdp++;
        }
      }
      else
        break;
    }
    free(cmds);
  }
}

#ifdef pathname_conversion_here
#warning now unused
/*
   used by win_open
*/
wstring
child_conv_path(wstring wpath, bool adjust_dir)
{
  int wlen = wcslen(wpath);
  int len = wlen * cs_cur_max;
  char path[len];
  len = cs_wcntombn(path, wpath, len, wlen);
  path[len] = 0;

  char * exp_path;  // expanded path
  if (*path == '~') {
    // Tilde expansion
    char * name = path + 1;
    char * rest = strchr(path, '/');
    if (rest)
      *rest++ = 0;
    else
      rest = "";
    char * base;
    if (!*name)
      base = home;
    else {
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
      // Find named user's home directory
      struct passwd * pw = getpwnam(name);
      base = (pw ? pw->pw_dir : 0) ?: "";
#else
      // Pre-1.5 Cygwin simply copies HOME into pw_dir, which is no use here.
      base = "";
#endif
    }
    exp_path = asform("%s/%s", base, rest);
  }
  else if (*path != '/' && adjust_dir) {
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
    // Handle relative paths. Finding the foreground process working directory
    // requires the /proc filesystem, which isn't available before Cygwin 1.5.

    // Find pty's foreground process, if any. Fall back to child process.
    int fg_pid = (pty_fd >= 0) ? tcgetpgrp(pty_fd) : 0;
    if (fg_pid <= 0)
      fg_pid = pid;

    char * cwd = foreground_cwd();
    exp_path = asform("%s/%s", cwd ?: home, path);
    if (cwd)
      free(cwd);
#else
    // If we're lucky, the path is relative to the home directory.
    exp_path = asform("%s/%s", home, path);
#endif
  }
  else
    exp_path = path;

# if CYGWIN_VERSION_API_MINOR >= 222
  // CW_INT_SETLOCALE was introduced in API 0.222
  cygwin_internal(CW_INT_SETLOCALE);
# endif
  wchar *win_wpath = path_posix_to_win_w(exp_path);
  // Drop long path prefix if possible,
  // because some programs have trouble with them.
  if (win_wpath && wcslen(win_wpath) < MAX_PATH) {
    wchar *old_win_wpath = win_wpath;
    if (wcsncmp(win_wpath, W("\\\\?\\UNC\\"), 8) == 0) {
      win_wpath = wcsdup(win_wpath + 6);
      win_wpath[0] = '\\';  // Replace "\\?\UNC\" prefix with "\\"
      free(old_win_wpath);
    }
    else if (wcsncmp(win_wpath, W("\\\\?\\"), 4) == 0) {
      win_wpath = wcsdup(win_wpath + 4);  // Drop "\\?\" prefix
      free(old_win_wpath);
    }
  }

  if (exp_path != path)
    free(exp_path);

  return win_wpath;
}
#endif

void
child_set_fork_dir(char * dir)
{
  strset(&child_dir, dir);
}

void
setenvi(char * env, int val)
{
  static char valbuf[22];  // static to prevent #530
  sprintf(valbuf, "%d", val);
  setenv(env, valbuf, true);
}

static void
setup_sync(bool in_tabs)
{
  if (sync_level()) {
    if (win_is_fullscreen) {
      setenvi("IMINTTY_DX", 0);
      setenvi("IMINTTY_DY", 0);
    }
    else if (!IsZoomed(wnd)) {
      RECT r;
      GetWindowRect(wnd, &r);
      setenvi("IMINTTY_X", r.left);
      setenvi("IMINTTY_Y", r.top);
      setenvi("IMINTTY_DX", r.right - r.left);
      setenvi("IMINTTY_DY", r.bottom - r.top);
    }
    if (cfg.tabbar) {
      setenvi("IMINTTY_TABBAR", cfg.tabbar);
      setenvi("IMINTTY_SYNC", cfg.geom_sync);
      // enforce proper grouping, i.e. either new tab or window, as requested
      if (in_tabs && *cfg.class) {
        char * class = cs__wcstoutf(cfg.class);
        setenv("IMINTTY_CLASS", class, true);
        free(class);
      }
      else if (!in_tabs)
        setenv("IMINTTY_CLASS", "+", true);
    }
  }
}

/*
   Called from Alt+F2 (or session launcher via child_launch).
*/
static void
do_child_fork(int argc, char *argv[], int moni, bool launch, bool config_size, bool in_cwd, bool cloning, HWND embed_wnd)
{
  trace_dir(asform("do_child_fork: %s", getcwd(malloc(MAX_PATH), MAX_PATH)));

#ifdef control_AltF2_size_via_token
  void reset_fork_mode()
  {
    clone_size_token = true;
  }
#endif

#if defined(__MINGW32__) || defined(__MINGW64__)
  /* P8: MinGW path — use CreateProcess instead of fork() */
  (void)cloning;
  (void)embed_wnd;  /* embed handled separately */

  string set_dir = 0;
  if (in_cwd) {
    if (support_wsl) {
      if (child_dir && *child_dir)
        set_dir = strdup(child_dir);
    }
    else
      set_dir = get_foreground_cwd();
  }

  if ((child_dir && *child_dir) || set_dir) {
    if (!set_dir)
      set_dir = guardpath(child_dir, 2);
    if (set_dir) {
      wchar_t wdir[MAX_PATH];
      if (MultiByteToWideChar(CP_UTF8, 0, set_dir, -1, wdir, MAX_PATH) > 0)
        SetCurrentDirectoryW(wdir);
      mingw_setenv("PWD", set_dir, true);
      if (!launch) {
        mingw_setenv("CHERE_INVOKING", "imintty", true);
        if (shortcut)
          mingw_setenv("IMINTTY_PWD", set_dir, true);
      }
      free((char *)set_dir);
    }
  }

  /* Build command line for CreateProcess */
  int total_len = 0;
  for (int i = 0; i < argc; i++) total_len += (int)strlen(argv[i]) + 2;
  wchar_t *cmdline_w = newn(wchar_t, (size_t)total_len + 1);
  wchar_t *p = cmdline_w;
  for (int i = 0; i < argc; i++) {
    char *aq = strchr(argv[i], ' ');
    if (aq) {
      *p++ = L'"';
      int n = (int)(aq - argv[i]);
      for (int j = 0; j < n; j++) *p++ = (wchar_t)argv[i][j];
      *p++ = L'"';
    } else {
      int n = (int)strlen(argv[i]);
      for (int j = 0; j < n; j++) *p++ = (wchar_t)argv[i][j];
    }
    *p++ = L' ';
  }
  *p = L'\0';

  /* Set env vars */
  if (!config_size) {
    char buf[32];
    sprintf(buf, "%d", term.rows0);
    mingw_setenv("IMINTTY_ROWS", buf, true);
    sprintf(buf, "%d", term.cols0);
    mingw_setenv("IMINTTY_COLS", buf, true);
  }
  if (moni > 0) {
    char buf[16];
    sprintf(buf, "%d", moni);
    mingw_setenv("IMINTTY_MONITOR", buf, true);
  }
  if (win_is_fullscreen)
    mingw_setenv("IMINTTY_MAXIMIZE", "2", true);
  else if (IsZoomed(wnd))
    mingw_setenv("IMINTTY_MAXIMIZE", "1", true);
  if (icon_is_from_shortcut) {
    char *ico = cs__wcstoutf(cfg.icon);
    mingw_setenv("IMINTTY_ICON", ico, true);
    free(ico);
  }

  // Inject --embed for container-embedded tabs (MinGW path)
  if (embed_wnd) {
    char embed_str[32];
    sprintf(embed_str, "--embed %p", (void *)embed_wnd);
    wchar_t *ew = cs__mbstowcs(embed_str);
    if (ew) {
      int elenw = (int)wcslen(ew);
      int oldlen = (int)wcslen(cmdline_w);
      cmdline_w = renewn(cmdline_w, oldlen + elenw + 2);
      cmdline_w[oldlen] = L' ';
      for (int k = 0; k <= elenw; k++)
        cmdline_w[oldlen + 1 + k] = ew[k];
      free(ew);
    }
  }

  /* Launch via CreateProcessW */
  STARTUPINFOW si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  wchar_t exe_path[MAX_PATH];
  GetModuleFileNameW(NULL, exe_path, MAX_PATH);

  if (CreateProcessW(NULL, cmdline_w, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
    pid = (pid_t)pi.dwProcessId;
    CloseHandle(pi.hThread);
  } else {
    childerror(_("Error: Could not create child process"), true, 0, 0);
  }
  free(cmdline_w);
#else
  pid_t clone = fork();

  if (cfg.daemonize) {
    if (clone < 0) {
      childerror(_("Error: Could not fork child daemon"), true, errno, 0);
      return;
    }
    if (clone > 0) {
      int status;
      waitpid(clone, &status, 0);
      return;
    }

    clone = fork();
    if (clone < 0) {
      exit(mexit);
    }
    if (clone > 0) {
      exit(0);
    }
  }

  if (clone == 0) {  // prepare child process to spawn new terminal
    string set_dir = 0;
    if (in_cwd) {
      if (support_wsl) {
        if (child_dir && *child_dir)
          set_dir = strdup(child_dir);
      }
      else
        set_dir = get_foreground_cwd();  // do this before close(pty_fd)!
    }

    if (pty_fd >= 0)
      close(pty_fd);
    if (log_fd >= 0)
      close(log_fd);  // close file handle in forked child, no need to flush
    close(win_fd);

    if ((child_dir && *child_dir) || set_dir) {
      if (set_dir) {
        // use cwd of foreground process if requested via in_cwd
      }
#ifdef pathname_conversion_here
#warning now deprecated; handled via guardpath
      else if (support_wsl) {
        wchar * wcd = cs__utftowcs(child_dir);
#ifdef debug_wsl
        printf("fork wsl <%ls>\n", wcd);
#endif
        wcd = dewsl(wcd);
#ifdef debug_wsl
        printf("fork wsl <%ls>\n", wcd);
#endif
        set_dir = (string)cs__wcstombs(wcd);
        delete(wcd);
      }
#endif
      else {
        //set_dir = strdup(child_dir);
        set_dir = guardpath(child_dir, 2);
        if (!set_dir)
          sleep(1);  // let beep be audible before execv below
          // also skip chdir below (guarded)
      }

      if (set_dir) {
        chdir(set_dir);
        trace_dir(asform("child: %s", set_dir));
        setenv("PWD", set_dir, true);  // avoid softlink resolution
        // prevent shell startup from setting current directory to $HOME
        // unless cloned/Alt+F2 (!launch)
        if (!launch) {
          setenv("CHERE_INVOKING", "imintty", true);
          // if cloned and then launched from Windows shortcut (!shortcut) 
          // (by sanitizing taskbar icon grouping, #784, imintty/wsltty#96) 
          // indicate to set proper directory
          if (shortcut)
            setenv("IMINTTY_PWD", set_dir, true);
        }

        delete(set_dir);
      }
    }

#ifdef add_child_parameters
    // add child parameters
    int newparams = 0;
    char * * newargv = malloc((argc + newparams + 1) * sizeof(char *));
    int i = 0, j = 0;
    bool addnew = true;
    while (1) {
      if (addnew && (! argv[i] || strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "-") == 0)) {
        addnew = false;
        // insert additional parameters here
        newargv[j++] = "-o";
        static char parbuf1[28];  // static to prevent #530
        sprintf(parbuf1, "Rows=%d", term.rows);
        newargv[j++] = parbuf1;
        newargv[j++] = "-o";
        static char parbuf2[31];  // static to prevent #530
        sprintf(parbuf2, "Columns=%d", term.cols);
        newargv[j++] = parbuf2;
      }
      newargv[j] = argv[i];
      if (! argv[i])
        break;
      i++;
      j++;
    }
    argv = newargv;
#else
    (void) argc;
#endif

    // Inject --embed for container-embedded tabs
    if (embed_wnd) {
      char embed_str[32];
      sprintf(embed_str, "%p", (void *)embed_wnd);
      int new_argc = argc + 2;
      char **newargv = (char **)malloc((size_t)new_argc + 1 * sizeof(char *));
      if (newargv) {
        newargv[0] = argv[0];
        newargv[1] = "--embed";
        newargv[2] = embed_str;
        int k = 3;
        for (int i = 1; argv[i]; i++, k++)
          newargv[k] = argv[i];
        newargv[k] = NULL;
        argv = newargv;
      }
    }

    // provide environment to clone size
    if (!config_size) {
      setenvi("IMINTTY_ROWS", term.rows0);
      setenvi("IMINTTY_COLS", term.cols0);
#ifdef support_horizontal_scrollbar_with_tabbar
      // this does not work, so horizontal scrollbar is disabled with tabbar
extern int horsqueeze(void);  // should become horsqueeze_cols in win.h
      setenvi("IMINTTY_SQUEEZE", horsqueeze() / cell_width);
#endif
      // provide environment to maximise window
      if (win_is_fullscreen)
        setenvi("IMINTTY_MAXIMIZE", 2);
      else if (IsZoomed(wnd))
        setenvi("IMINTTY_MAXIMIZE", 1);
    }
    // provide environment to select monitor
    if (moni > 0)
      setenvi("IMINTTY_MONITOR", moni);
    // propagate shortcut-inherited icon
    if (icon_is_from_shortcut)
      setenv("IMINTTY_ICON", cs__wcstoutf(cfg.icon), true);

    //setenv("IMINTTY_CHILD", "1", true);

#if CYGWIN_VERSION_DLL_MAJOR >= 1005
    if (cloning && shortcut) {
      trace_dir(asform("Starting <%s>", cs__wcstoutf(shortcut)));
      shell_exec(shortcut);
      //show_info("Started");
      sleep(5);  // let starting settle, or it will fail; 1s normally enough
      exit(0);
    }

    trace_dir(asform("Starting exe <%s %s>", argv[0], argv[1]));
    execv("/proc/self/exe", argv);
#else
    (void)cloning;
    // /proc/self/exe isn't available before Cygwin 1.5, so use argv[0] instead.
    // Strip enclosing quotes if present.
    char *path = argv[0];
    int len = strlen(path);
    if (path[0] == '"' && path[len - 1] == '"') {
      path = strdup(path + 1);
      path[len - 2] = 0;
    }
    execvp(path, argv);
#endif
    exit(mexit);
  }
  //reset_fork_mode();
#endif /* __MINGW32__ || __MINGW64__ */
}

/*
  Called from Alt+F2.
 */
void
child_fork(int argc, char *argv[], int moni, bool config_size, bool in_cwd, bool in_tabs, HWND embed_wnd)
{
  setup_sync(in_tabs);
  do_child_fork(argc, argv, moni, false, config_size, in_cwd, true, embed_wnd);
  unsetenv("IMINTTY_CLASS");
}

/*
  Called from session launcher.
 */
void
child_launch(int n, int argc, char * argv[], int moni)
{
  setup_sync(true);

  if (*cfg.session_commands) {
    char * cmds = cs__wcstombs(cfg.session_commands);
    char * cmdp = cmds;
    char sepch = ';';
    if ((uchar)*cmdp <= (uchar)' ')
      sepch = *cmdp++;

    char * paramp;
    while (n >= 0 && (paramp = strchr(cmdp, ':'))) {
      paramp++;
      char * sepp = strchr(paramp, sepch);
      if (sepp)
        *sepp = '\0';

      if (n == 0) {
        //setup_sync(false);
        argc = 1;
        char ** new_argv = newn(char *, argc + 1);
        new_argv[0] = argv[0];
        // prepare launch parameters from config string
        while (*paramp) {
          while (*paramp == ' ')
            paramp++;
          if (*paramp) {
            new_argv = renewn(new_argv, argc + 2);
            new_argv[argc] = paramp;
            argc++;
            while (*paramp && *paramp != ' ')
              paramp++;
            if (*paramp == ' ')
              *paramp++ = '\0';
          }
        }
        new_argv[argc] = 0;
        do_child_fork(argc, new_argv, moni, true, true, false, false, NULL);
        free(new_argv);
        break;
      }
      n--;

      if (sepp) {
        cmdp = sepp + 1;
        // check for multi-line separation
        if (*cmdp == '\\' && cmdp[1] == '\n') {
          cmdp += 2;
          while (iswspace(*cmdp))
            cmdp++;
        }
      }
      else
        break;
    }
    free(cmds);
  }
}

