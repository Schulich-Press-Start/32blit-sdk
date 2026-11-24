"""Smoke the real CMake import/staging helpers in a disposable native project.

Run: python tools/blit-tools/tests/test_tiled_staging.py
Requires Python 3.9+, CMake 3.21+, Ninja (or CMAKE_GENERATOR), and a native
C++ compiler. No SDK build, Tiled installation, or third-party Python modules
are needed; only the fixture's scene-data target is built.
"""

import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib


SDK = Path(__file__).resolve().parents[3]
MAPS = ("maps/one.tmx", "maps/two.tmx")


def atlas_png(color):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    pixels = (b"\0" + bytes(color) * 8) * 8
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


class TiledStagingSmoke(unittest.TestCase):
    def write_source(self, relative, content):
        path = self.project / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            path.write_text(content, encoding="utf-8")
        return path

    def run_cmake(self, *arguments):
        command = ["cmake", *map(str, arguments)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0,
                         f"{' '.join(command)}\n{result.stdout}\n{result.stderr}")

    def build_data(self):
        self.run_cmake("--build", self.build, "--config", "Debug",
                       "--target", "smoke_scene_data", "--parallel", "8")

    def write_tileset(self, solid):
        self.write_source("maps/atlas.tsx", f"""<tileset tilewidth="8" tileheight="8" tilecount="1" columns="1">
  <image source="../textures/atlas.png" width="8" height="8"/>
  <tile id="0"><properties>
    <property name="solid" type="bool" value="{str(solid).lower()}"/>
  </properties></tile>
</tileset>
""")

    def setUp(self):
        # Spaces exercise the depfile paths as well as quoted CMake commands.
        temporary = tempfile.TemporaryDirectory(prefix="blit tiled staging ")
        self.addCleanup(temporary.cleanup)
        self.project = Path(temporary.name) / "project"
        self.build = self.project / "build"
        self.data = self.build / "bin" / "Debug" / "data"
        self.project.mkdir()
        self.write_source("blit-project.json", json.dumps({
            "version": 1, "registry": "contracts/components.json"}))
        self.write_source("contracts/components.json", json.dumps({
            "version": 1, "components": {}}))
        self.write_source("authoring.schema.json", json.dumps({
            "version": 1, "classes": {}, "tile_flags": {"solid": 0}}))
        self.write_source("textures/atlas.png", atlas_png((255, 0, 0, 255)))
        self.write_source("assets/readme.txt", "Conventional asset trees are kept whole.\n")
        self.write_source("scenes/start.scene", "scene 1 Start\n")
        self.write_source("unrelated.props", "Not a map companion.\n")
        self.write_tileset(True)
        for source in (*MAPS, "maps/unstaged.tmx"):
            self.write_source(source, """<map orientation="orthogonal" width="1" height="1" tilewidth="8" tileheight="8">
  <tileset firstgid="1" source="atlas.tsx"/>
  <layer name="Ground" width="1" height="1"><data encoding="csv">1</data></layer>
</map>
""")
        self.write_source("main.cpp", "int main() { return 0; }\n")
        # Stand-in link targets avoid SDL/world dependencies; both CMake helpers
        # and the importer are the real SDK implementations, not test copies.
        self.write_source("CMakeLists.txt", f"""cmake_minimum_required(VERSION 3.21)
project(tiled_staging_smoke LANGUAGES CXX)
add_library(BlitHalSDL INTERFACE)
add_library(BlitGameScene INTERFACE)
function(blit_executable target)
    add_executable(${{target}} ${{ARGN}})
endfunction()
include("{SDK.as_posix()}/libs/game_scene/SceneGame.cmake")
include("{SDK.as_posix()}/tools/blit-tools/Tiled.cmake")
blit_scene_game(smoke SOURCES main.cpp)
set_target_properties(smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/bin/$<CONFIG>")
foreach(map IN ITEMS maps/one.tmx maps/two.tmx)
    blit_tiled_assets(smoke "${{map}}" authoring.schema.json
        "${{CMAKE_CURRENT_SOURCE_DIR}}/contracts/components.json")
endforeach()
# An ordinary game without scene staging must still configure successfully.
add_executable(unstaged EXCLUDE_FROM_ALL main.cpp)
blit_tiled_assets(unstaged maps/unstaged.tmx authoring.schema.json
    "${{CMAKE_CURRENT_SOURCE_DIR}}/contracts/components.json")
""")
        self.run_cmake("-S", self.project, "-B", self.build,
                       "-G", os.environ.get("CMAKE_GENERATOR", "Ninja"),
                       "-DCMAKE_BUILD_TYPE=Debug", f"-DPython_EXECUTABLE={sys.executable}")

    def assert_generated_staged(self, solid, include_asset=True):
        expected = {"blit-project.json", "contracts", "contracts/components.json",
                    "textures", "textures/atlas.png", "scenes", "scenes/start.scene",
                    "generated", "generated/maps"}
        if include_asset:
            expected.update(("assets", "assets/readme.txt"))
        # Import intentionally keeps earlier content-addressed generations.
        expected.update(path.relative_to(self.project).as_posix()
                        for path in (self.project / "generated/maps").iterdir())
        inventory = set((self.data / ".blit-scene-files").read_text(encoding="utf-8").splitlines())
        self.assertEqual(inventory, expected)
        names = []
        for source in MAPS:
            scene = "generated/" + str(Path(source).with_suffix(".generated.scene")).replace("\\", "/")
            text = (self.data / scene).read_text(encoding="utf-8")
            names.append(next(line[len("# blit-map "):] for line in text.splitlines()
                              if line.startswith("# blit-map ")))
            packed = (self.data / names[-1]).read_bytes()
            props = (self.data / (names[-1] + ".props")).read_bytes()
            self.assertEqual(packed[:4], b"BTMP")
            self.assertEqual(props[:4], b"BTP1")
            self.assertEqual(struct.unpack_from("<I", props, 16)[0], int(solid))
        return names


    def test_clean_parallel_and_incremental_staging(self):
        self.assertFalse((self.project / "generated").exists())
        # Building staging directly makes a missing import edge fail reliably,
        # rather than hoping to win the executable's original parallel race.
        self.build_data()
        original_maps = self.assert_generated_staged(True)
        self.assertFalse((self.project / "generated/maps/unstaged.generated.scene").exists())

        # Only the external TSX changes: its depfile must rerun both imports,
        # and staging must observe the new map/props pair in the same build.
        time.sleep(1.1)
        self.write_tileset(False)
        self.build_data()
        updated_maps = self.assert_generated_staged(False)
        for old, new in zip(original_maps, updated_maps):
            self.assertNotEqual(old, new)

        # Atlas bytes are not consumed by import, but art-only edits must stage
        # even when the executable and generated scene are already up to date.
        time.sleep(1.1)
        self.write_source("textures/atlas.png", atlas_png((0, 0, 255, 255)))
        self.build_data()
        self.assertEqual(self.assert_generated_staged(False), updated_maps)

        # Inventory pruning includes old companions, but not unmanaged files.
        (self.data / "local.txt").write_text("Keep me.\n", encoding="utf-8")
        for old in original_maps:
            (self.project / old).unlink()
            (self.project / (old + ".props")).unlink()
        (self.project / "assets/readme.txt").unlink()
        self.build_data()
        self.assert_generated_staged(False, include_asset=False)
        for old in original_maps:
            self.assertFalse((self.data / old).exists())
            self.assertFalse((self.data / (old + ".props")).exists())
        self.assertFalse((self.data / "assets").exists())
        self.assertEqual((self.data / "local.txt").read_text(encoding="utf-8"), "Keep me.\n")


if __name__ == "__main__":
    unittest.main()
