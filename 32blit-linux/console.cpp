/**
 * \file console.cpp
 * \brief Text console handling (graphics mode, echo, signals).
 *
 * `KDSETMODE` with `KD_GRAPHICS` tells the kernel's framebuffer console to
 * stop drawing on the virtual terminal; `KD_TEXT` gives it the screen back
 * and makes it redraw. It only works on a real virtual terminal (not over
 * SSH) and needs access to it, so we try stdin first, then `/dev/tty0`
 * (the active VT, usually root only).
 */
#include "console.hpp"

#include <csignal>
#include <cstdio>
#include <initializer_list>

#include <fcntl.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace blit_linux {

  namespace {
    /// Virtual terminal we switched to graphics mode, or -1.
    int vt_fd = -1;
    /// True if we opened vt_fd ourselves and must close it.
    bool vt_fd_owned = false;
    /// Mode to restore (normally KD_TEXT).
    int saved_kd_mode = KD_TEXT;

    bool termios_saved = false;
    /// stdin settings before we turned echo off.
    termios saved_termios;

    volatile sig_atomic_t quit_flag = 0;
    volatile sig_atomic_t restored = 0;

    /** \brief SIGINT/SIGTERM/SIGHUP: ask the main loop to finish so normal cleanup runs. */
    void on_quit_signal(int) {
      quit_flag = 1;
    }

    /** \brief Crash signals: restore the console, then let the default action (core dump) happen. */
    void on_fatal_signal(int sig) {
      console_restore();
      signal(sig, SIG_DFL);
      raise(sig);
    }

    /** \brief True if `fd` is a virtual terminal; its current KD mode is written to `mode`. */
    bool is_vt(int fd, int &mode) {
      return fd >= 0 && ioctl(fd, KDGETMODE, &mode) == 0;
    }
  }

  void console_init() {
    // 1. graphics mode on the virtual terminal
    int mode;
    if(is_vt(STDIN_FILENO, mode)) {
      vt_fd = STDIN_FILENO;
    } else {
      int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
      if(is_vt(fd, mode)) {
        vt_fd = fd;
        vt_fd_owned = true;
      } else if(fd >= 0) {
        close(fd);
      }
    }

    if(vt_fd >= 0 && ioctl(vt_fd, KDSETMODE, KD_GRAPHICS) == 0) {
      saved_kd_mode = mode;
    } else {
      if(vt_fd_owned)
        close(vt_fd);
      vt_fd = -1;
      vt_fd_owned = false;
      fprintf(stderr, "32blit-linux: can't hide the text console (run from a virtual terminal, or as root); text may draw over the game\n");
    }

    // 2. no echo / line editing, so keys pressed during play don't end up in the shell.
    //    ISIG stays on so Ctrl+C still sends SIGINT.
    if(isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
      termios raw = saved_termios;
      raw.c_lflag &= ~tcflag_t(ECHO | ICANON);
      if(tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
        termios_saved = true;
    }

    // 3. signals
    struct sigaction quit_action = {};
    quit_action.sa_handler = on_quit_signal;
    sigemptyset(&quit_action.sa_mask);
    sigaction(SIGINT, &quit_action, nullptr);
    sigaction(SIGTERM, &quit_action, nullptr);
    sigaction(SIGHUP, &quit_action, nullptr);

    for(int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT})
      signal(sig, on_fatal_signal);
  }

  void console_restore() {
    if(restored)
      return;
    restored = 1;

    // only async-signal-safe calls below (ioctl, tcflush, tcsetattr, close)
    if(vt_fd >= 0) {
      ioctl(vt_fd, KDSETMODE, saved_kd_mode);
      if(vt_fd_owned)
        close(vt_fd);
      vt_fd = -1;
    }

    if(termios_saved) {
      tcflush(STDIN_FILENO, TCIFLUSH); // drop keys typed during play
      tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    }
  }

  bool quit_requested() {
    return quit_flag != 0;
  }
}
