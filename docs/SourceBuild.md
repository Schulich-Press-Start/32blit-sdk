# Optional Pong source builds and Dev

Skip this if you only want to follow the editor tutorial. The tutorial project has authored assets plus a prebuilt game; use **Play** after saving. To change C++, work in the SDK checkout's `examples/team-pong` source project. Use the kit's matching SDK source. The optional build instructions below do not alter the packaged examples.

## MSYS2 UCRT64 / MinGW

Open **MSYS2 UCRT64**, not the plain MSYS shell. Install the native compiler, CMake, Ninja, Python, and matching SDL libraries:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,SDL2,SDL2_image,SDL2_net,python,python-pip}
python -m venv .venv
source .venv/Scripts/activate
python -m pip install '32blit==0.7.4' 'bitstring<5' -e ../../tools/blit-tools
cmake --preset windows-mingw
cmake --build build-mingw --target team_pong
./build-mingw/Debug/team_pong.exe
```

Run the commands from `examples/team-pong`. Run the game from the same shell so it can find UCRT64's runtime and SDL DLLs on `PATH`. Do not reuse a Visual Studio build directory or pass the MSVC SDL directory (`vs/sdl`) to this preset. If CMake cannot find SDL, check that `$MINGW_PREFIX` is set and that the installed packages are the UCRT64 variants.

For **optional Dev** using MinGW, configure first with the preset above, stop any Play instance, then run this from the same UCRT64 shell:

```bash
blit dev --build-dir build-mingw --target team_pong --exe build-mingw/Debug/team_pong.exe --build-on-save --scene scenes/pong.scene --scene-watch scenes/pong.map --scene-watch scenes/pong.map.json --scene-watch assets/court.tileset.json --scene-watch assets/paddle.sprite.json --scene-watch assets/ball.sprite.json
```

The editor's default Windows Dev settings are for MSVC. Use the command above for MinGW rather than mixing build folders or environments.

## Visual Studio / MSVC

Install CMake, Python, and **Desktop development with C++** in Visual Studio or Build Tools. Use Developer PowerShell in `examples/team-pong`:

```powershell
$sdk = (Resolve-Path ../..).Path
py -m pip install '32blit==0.7.4' 'bitstring<5' -e "$sdk/tools/blit-tools"
cmake --preset windows "-D32BLIT_DIR=$sdk" "-DSDL2_DIR=$sdk/vs/sdl"
cmake --build build-windows --config Debug --target team_pong
.\build-windows\Debug\team_pong.exe
```

After configuring once, open this source project in the editor and use **Dev** if `blit` is on `PATH`. No SDK or compiler is needed to run the ZIP's prebuilt game.
