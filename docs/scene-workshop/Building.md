# Build the same scene as a standalone game

Build the project you just created. **New Project** already supplied `game.cpp`, `CMakeLists.txt` and compiler presets. The target is **`game`**. Choose one compiler. You need internet to install the public compiler tools, but the kit includes the SDK source and scene dependencies. Scene projects use PNGs directly: no Python, asset packer, or `assets.yml` is needed. Paths with spaces are fine.

## Option A: Visual Studio Build Tools (MSVC)

1. Install [Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/) with **Desktop development with C++**, the MSVC x64/x86 tools and a Windows SDK. The full Visual Studio IDE is not required.
2. Install [CMake](https://cmake.org/download/) with **Add CMake to PATH**. Open **Developer PowerShell for Visual Studio** and check `where.exe cl` and `cmake --version`.
3. Replace the first path below with the extracted kit folder. Keep this terminal for later checkpoints.

```powershell
Set-Location 'C:\Games\Scene Workshop'
$kit = (Get-Location).Path
$sdk = Join-Path $kit '32blit-sdk'
Set-Location "$kit/My Game"
cmake --preset windows "-D32BLIT_DIR=$sdk" "-DSDL2_DIR=$sdk/vs/sdl"
cmake --build build-windows --config Debug --target game --parallel 4
.\build-windows\Debug\game.exe
```

## Option B: MSYS2 UCRT64 (MinGW GCC)

1. Install [MSYS2](https://www.msys2.org/). Open **MSYS2 UCRT64**, not the plain MSYS shell. Run `pacman -Syu`; if asked to close the terminal, reopen UCRT64 and run it again. Install the matching packages:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,SDL2,SDL2_image,SDL2_net}
```

2. Check `echo "$MSYSTEM"` prints `UCRT64` and `which g++ cmake ninja` shows `/ucrt64/bin/…`. Replace the first path below with yours. Stay in this shell so the compiler and matching DLLs are on `PATH`.

```bash
cd '/c/Games/Scene Workshop'
kit="$PWD"
sdk="$(cygpath -m "$kit/32blit-sdk")"
cd "$kit/My Game"
cmake --preset windows-mingw "-D32BLIT_DIR=$sdk" "-DCMAKE_PREFIX_PATH=$(cygpath -m "$MINGW_PREFIX")"
cmake --build build-mingw --target game --parallel 4
./build-mingw/Debug/game.exe
```

## Check your standalone game

**Check:** The built game shows the same art and animation as **Play Scene**. After the first configure, repeat just your compiler's **build and run** lines after a source change. Rebuild after an art edit to update the standalone data. Close the executable first on Windows. Do not reuse one compiler's build directory for the other.

For distribution use a **Release** build. For MSVC, build target `game` with `--config Release` and run `cmake --install build-windows --config Release --prefix dist`. For MinGW, configure the same preset in a separate directory with `-B build-mingw-release -DCMAKE_BUILD_TYPE=Release`, then build and install that directory with `--prefix dist`. Share **all of `dist/bin/`**: executable, matching runtime DLLs, and `data/`, not just `game.exe`. Keep dependency licenses with your distribution. Do not distribute the MSVC Debug runtime.

**MSVC / Developer PowerShell**, using the configured build above:

```powershell
cmake --build build-windows --config Release --target game --parallel 4
cmake --install build-windows --config Release --prefix dist
```

**MSYS2 UCRT64**, from the project with `$sdk` set as above:

```bash
cmake --preset windows-mingw -B build-mingw-release -DCMAKE_BUILD_TYPE=Release "-D32BLIT_DIR=$sdk" "-DCMAKE_PREFIX_PATH=$(cygpath -m "$MINGW_PREFIX")"
cmake --build build-mingw-release --target game --parallel 4
cmake --install build-mingw-release --prefix dist
```

Try launching the distributed game outside the project folder. The standalone `data` includes its own manifest, scene and asset files. Add another sprite in the editor, save, rebuild and check again: no list of image filenames needs updating. To draw custom art or a scoreboard over the scene, keep using the ordinary 32blit drawing API after the scene draw call.
