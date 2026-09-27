// mingwcompat.h (part of imintty)
// P8: MinGW-w64 POSIX compatibility subset, force-included via std.h.
// Licensed under the terms of the GNU General Public License v3 or later.

#ifndef MINGWCOMPAT_H
#define MINGWCOMPAT_H

#ifndef MINGW_NATIVE
#error "mingwcompat.h included without MINGW_NATIVE"
#endif

#include <signal.h>     // mingw: SIGINT..SIGTERM, SIG_DFL, SIG_IGN, signal()
#include <time.h>       // localtime, strftime
#include <sys/time.h>   // struct timeval, gettimeofday
#include <fcntl.h>      // open, O_* flags
#include <process.h>    // _getpid
#include <sys/stat.h>   // struct stat, fstat
#include <direct.h>     // _mkdir, _chdir

/* signals not provided by default by mingw <signal.h> (guarded by _POSIX there) */
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGBUS
#define SIGBUS 10
#endif
#ifndef SIGSYS
#define SIGSYS 12
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGSTOP
#define SIGSTOP 19
#endif
#ifndef SIGTSTP
#define SIGTSTP 20
#endif
#ifndef SIGTTIN
#define SIGTTIN 21
#endif
#ifndef SIGTTOU
#define SIGTTOU 22
#endif

/* exit code to use for failure of exec */
#ifndef mexit
#define mexit 126
#endif

/* POSIX struct winsize (same guard as child.h) */
#ifndef IMINTTY_WINSIZE_DEFINED
#define IMINTTY_WINSIZE_DEFINED
struct winsize {
  unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};
#endif

/* minimal termios emulation (no tty layer under MinGW) */
#define NCCS 32
struct termios {
  unsigned c_iflag, c_oflag, c_cflag, c_lflag;
  unsigned char c_cc[NCCS];
};

#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSWTC 7
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define VEOL 11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT 15
#define VEOL2 16

#define IGNBRK 0000001
#define BRKINT 0000002
#define IXANY 0002000
#define IMAXBEL 00020000
#define ECHO 0000010
#define ICANON 0000002
#define ECHOE 0000020
#define ECHOK 0000040
#define ECHOCTL 0001000
#define ECHONL 0000100
#define ECHOKE 000200000
#define ISIG 0000001
#define IEXTEN 0100000

#define TCSADRAIN 1
#define TCSANOW 0
#define TCSAFLUSH 2

#define TIOCSWINSZ 0x5414

#define CTRL(c) ((c) & 0x1F)
#define CDEL 0x7F        /* cygwin-style delete character */
#define CERASE CDEL

/* flags not available on MinGW */
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif

/* readlink is not available on MinGW; /proc paths do not exist anyway */
static inline int
readlink(const char * path, char * buf, size_t count)
{
  (void)path;
  (void)buf;
  (void)count;
  return -1;
}

/* waitpid status interpretation (status = exitcode << 8) */
#define WNOHANG 1
#define WUNTRACED 2
#define WIFEXITED(s) (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s) ((((s) & 0x7F) != 0) && (((s) & 0x7F) != 0x7F))
#define WTERMSIG(s) ((s) & 0x7F)
extern int waitpid(int pid, int * status, int options);

/* replacements for missing POSIX functions (implemented in std.c) */
extern void mingw_usleep(unsigned usec);
#define usleep(us) mingw_usleep(us)
extern int kill(int pid, int sig);
extern char * strsignal(int sig);
extern int mingw_gethostname(char * name, int len);
extern int tcgetattr(int fd, struct termios * t);
extern int tcsetattr(int fd, int optional_actions, const struct termios * t);
extern int tcgetpgrp(int fd);
extern int ioctl(int fd, unsigned long request, ...);

/* password database subset (getpwuid/getpwnam replacements) */
struct passwd {
  char * pw_name;
  char * pw_passwd;
  char * pw_dir;
  char * pw_gecos;
  char * pw_shell;
};
extern struct passwd * getpwuid(int uid);
extern struct passwd * getpwnam(const char * name);
#define getuid() 0

#endif
