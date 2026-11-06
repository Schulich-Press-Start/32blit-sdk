# 32blit-linux

A port of the 32blit SDK to Linux that draws straight to the framebuffer: no window system, no SDL. 32blit games build for it unchanged, from the same project folder as the desktop (SDL) build.

| Part | Uses |
|---|---|
| Screen | `/dev/fb0` (16- or 32-bit true colour, any channel order) |
| Buttons | Every input device under `/dev/input` (keyboards, gamepads, devicetree `gpio-keys`) |
| Files and saves | POSIX file calls |
| Time | `CLOCK_MONOTONIC` |

## Building a game

The 32blit tools must be installed. Pin `bitstring`: 32blit tools 0.7.4 fail with bitstring 5.

```sh
python3 -m pip install 32blit "bitstring<5"
```

Then configure with `-D32BLIT_LINUX=ON`. For example, the 32blit examples, cloned into the folder that contains `32blit-sdk` (they look for the SDK at `../32blit-sdk`):

```sh
git clone https://github.com/32blit/32blit-examples
cmake -S 32blit-examples -B build -D32BLIT_LINUX=ON
cmake --build build -j
```

Without the flag, the same project builds for the desktop (SDL) instead.

## Running a game

Run it from a text console (a virtual terminal, e.g. Ctrl+Alt+F3), with nothing else drawing on the screen:

```sh
./build/sprite-test/sprite-test
```

Permissions:
- `/dev/fb0` needs the `video` group;
- `/dev/input/event*` needs the `input` group:

```sh
# then log out and back in
sudo usermod -aG video,input $USER
```

Over SSH or from a desktop terminal the game still runs, but the console can't be hidden, so console text may draw over the game (a message says so).

### Options

| Option | Meaning |
|---|---|
| `--save_dir <dir>` | Folder for this game's saves. Default: `<game dir>/saves/<title>/`. Relative paths are taken from the current directory; missing folders are created |
| `--launch_path <file>` | File for the game to open, as 32blit's launcher would pass |
| `--info` | Print the game's metadata and exit |
| `--help` | List the options and controls |

### Controls

| 32blit button | Keyboard | Gamepad (Linux codes) |
|---|---|---|
| D-pad | Arrows or WASD | `BTN_DPAD_*` |
| A / B / X / Y | Z X C V (or U I O P) | `BTN_SOUTH` / `BTN_EAST` / `BTN_WEST` / `BTN_NORTH` |
| HOME | 1 | `BTN_SELECT` |
| MENU | 2 or Esc | `BTN_START` |
| JOYSTICK (press) | 3 | `BTN_THUMBL` |

The keyboard layout matches the desktop (SDL) build.

### Quitting

Any of these quits the game:
- hold **HOME for one second**, as on the 32blit console;
- press **Ctrl+C**;
- the game calls the engine's exit function (`blit::api.exit(is_error)`).

The exit code is 1 if the game exited with an error, otherwise 0. The console is restored on every exit, including crashes and signals. Before handing the console back, the game waits until every key is released (at most 2 seconds), so a key still held, such as HOME after holding it to quit, doesn't keep repeating into the shell.

## Screen sizes

Without a size, `lores` is 160x120 and `hires` / `hires_palette` are 320x240. A game can ask for any other size from 96x96 up to the panel's size:

```cpp
// returns false, and keeps the current mode, if the size doesn't fit
set_screen_mode(ScreenMode::hires, PixelFormat::RGB, Size(480, 270));
```

The screen is shown at the largest whole-number scale that fits the panel, centred with black borders. For example, on a 480x272 panel:
- 160x120 shows at 2x;
- 320x240 and 480x270 show at 1x.

On a 640x480 panel, 320x240 shows at 2x and fills it.

## Files and saves

| What | Where |
|---|---|
| Game directory | The folder holding the executable. Relative paths a game opens are looked up here |
| Save directory | `--save_dir` if given, otherwise `<game dir>/saves/<game title>/`. The title comes from `metadata.yml`; `/` becomes `_`, and `.`, `..` or an empty title become `game` |

## Unit tests

Tests for the screen fit, pixel conversion and file API live in `tests/` and run on the build machine, no device needed:

```sh
cmake -S . -B build -D32BLIT_LINUX=ON -DBLIT_LINUX_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Add new tests as `tests/<name>_test.cpp` and list them in `tests/CMakeLists.txt`.

## Not supported yet

- **Audio:** the engine mixes it, but nothing plays it.
- **Vsync:** the framebuffer device can't page-flip, so frames can tear.
- **JPEG decoding:** `blit::decode_jpeg_*` crashes on this port (the `jpeg` example builds but crashes when it decodes).
- **Multiplayer:** the link always reports "not connected".
- **Launcher and flash functions:** not supported; games are started by a separate program.
