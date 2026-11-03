/**
 * \file input_evdev.cpp
 * \brief evdev implementation of the input functions.
 *
 * Each device delivers `struct input_event` records. For keys
 * (`type == EV_KEY`) the `value` is 1 for press, 0 for release and 2 for
 * auto-repeat (ignored). We keep one "held" bit per Linux key code across
 * all devices, then build the button mask from the mapping table. That way
 * releasing one of two keys mapped to the same button (e.g. Left arrow and
 * A) doesn't release the button while the other is still held.
 *
 * Devices are scanned once at startup; hot-plugging comes later.
 */
#include "input_evdev.hpp"

#include <bitset>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "engine/input.hpp"

namespace blit_linux {

  namespace {
    /** \brief One Linux key code and the 32blit button it presses. */
    struct KeyMapping {
      /// `KEY_*` / `BTN_*` from linux/input-event-codes.h.
      uint16_t code;
      /// `blit::Button` bit.
      uint32_t button;
    };

    /**
     * \brief Key code → button table, shared by every input device.
     *
     * Keyboard keys match the SDL port (32blit-sdl/Input.cpp) so desktop
     * and Linux builds play the same. Gamepad codes follow the Linux
     * gamepad convention (BTN_SOUTH = bottom face button, and so on).
     * Devicetree `gpio-keys` buttons should report these same codes, so no
     * device-specific table is needed.
     */
    constexpr KeyMapping key_map[] = {
      // keyboard: arrows and WASD
      {KEY_UP,    blit::Button::DPAD_UP},
      {KEY_DOWN,  blit::Button::DPAD_DOWN},
      {KEY_LEFT,  blit::Button::DPAD_LEFT},
      {KEY_RIGHT, blit::Button::DPAD_RIGHT},
      {KEY_W,     blit::Button::DPAD_UP},
      {KEY_S,     blit::Button::DPAD_DOWN},
      {KEY_A,     blit::Button::DPAD_LEFT},
      {KEY_D,     blit::Button::DPAD_RIGHT},

      // keyboard: action buttons (two sets, as in the SDL port)
      {KEY_Z, blit::Button::A},
      {KEY_X, blit::Button::B},
      {KEY_C, blit::Button::X},
      {KEY_V, blit::Button::Y},
      {KEY_U, blit::Button::A},
      {KEY_I, blit::Button::B},
      {KEY_O, blit::Button::X},
      {KEY_P, blit::Button::Y},

      // keyboard: system buttons
      {KEY_1,   blit::Button::HOME},
      {KEY_2,   blit::Button::MENU},
      {KEY_3,   blit::Button::JOYSTICK},
      {KEY_ESC, blit::Button::MENU},

      // gamepad buttons
      {BTN_DPAD_UP,    blit::Button::DPAD_UP},
      {BTN_DPAD_DOWN,  blit::Button::DPAD_DOWN},
      {BTN_DPAD_LEFT,  blit::Button::DPAD_LEFT},
      {BTN_DPAD_RIGHT, blit::Button::DPAD_RIGHT},
      {BTN_SOUTH,      blit::Button::A},
      {BTN_EAST,       blit::Button::B},
      {BTN_WEST,       blit::Button::X},
      {BTN_NORTH,      blit::Button::Y},
      {BTN_SELECT,     blit::Button::HOME},
      {BTN_START,      blit::Button::MENU},
      {BTN_THUMBL,     blit::Button::JOYSTICK},
    };

    /** \brief An open input device. */
    struct Device {
      /// Non-blocking file descriptor.
      int fd;
      /// e.g. /dev/input/event3, for log messages.
      std::string path;
    };

    std::vector<Device> devices;

    /// One bit per Linux key code: true while held on any device.
    std::bitset<KEY_CNT> held;

    /** \brief Test a bit in a bitmap returned by `EVIOCGBIT`. */
    bool test_bit(const unsigned long *bits, unsigned bit) {
      constexpr unsigned bits_per_long = sizeof(unsigned long) * 8;
      return (bits[bit / bits_per_long] >> (bit % bits_per_long)) & 1;
    }

    /** \brief True if the device can send at least one key in key_map. */
    bool has_mapped_key(int fd) {
      unsigned long key_bits[(KEY_CNT + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] = {};

      if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
        return false;

      for(auto &mapping : key_map) {
        if(test_bit(key_bits, mapping.code))
          return true;
      }

      return false;
    }
  }

  int input_init() {
    auto dir = opendir("/dev/input");
    if(!dir) {
      fprintf(stderr, "32blit-linux: cannot open /dev/input: %s\n", strerror(errno));
      return 0;
    }

    int denied = 0;

    while(auto ent = readdir(dir)) {
      if(strncmp(ent->d_name, "event", 5) != 0)
        continue;

      std::string path = std::string("/dev/input/") + ent->d_name;
      int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);

      if(fd < 0) {
        if(errno == EACCES)
          denied++;
        continue;
      }

      // skip mice, power buttons, lid switches and anything else without our keys
      if(!has_mapped_key(fd)) {
        close(fd);
        continue;
      }

      char name[128] = "unknown";
      ioctl(fd, EVIOCGNAME(sizeof(name)), name);
      printf("32blit-linux: input %s (%s)\n", path.c_str(), name);

      devices.push_back({fd, path});
    }

    closedir(dir);

    if(devices.empty()) {
      fprintf(stderr, "32blit-linux: no input devices found%s\n",
              denied ? " (permission denied: join the 'input' group or run as root)" : "");
    }

    return int(devices.size());
  }

  uint32_t input_poll() {
    input_event events[64];

    for(auto it = devices.begin(); it != devices.end();) {
      ssize_t len;

      while((len = read(it->fd, events, sizeof(events))) > 0) {
        int count = int(len / sizeof(input_event));

        for(int i = 0; i < count; i++) {
          auto &ev = events[i];

          // value 2 is auto-repeat; the button is already held
          if(ev.type == EV_KEY && ev.code < KEY_CNT && ev.value != 2)
            held[ev.code] = ev.value != 0;
        }
      }

      // device unplugged: drop it, and forget held keys so nothing stays stuck down
      if(len < 0 && errno == ENODEV) {
        fprintf(stderr, "32blit-linux: input %s removed\n", it->path.c_str());
        close(it->fd);
        it = devices.erase(it);
        held.reset();
        continue;
      }

      ++it;
    }

    uint32_t buttons = 0;
    for(auto &mapping : key_map) {
      if(held[mapping.code])
        buttons |= mapping.button;
    }

    return buttons;
  }

  void input_shutdown() {
    for(auto &device : devices)
      close(device.fd);

    devices.clear();
    held.reset();
  }
}
