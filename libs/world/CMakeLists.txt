# sdk::world - component registry, entities and scenes.
#
# Plain C++17 with no dependency on any port, on SDL or on the 32blit engine,
# so it builds for the PC, the handheld and 32blit hardware alike.
#
#   cmake -S . -B build -DBLIT_WORLD_TESTS=ON
#   cmake --build build
#   ctest --test-dir build --output-on-failure

# Also configurable on its own, so CI (and anyone working on the library) can
# build and test it without SDL or a port:
#
#   cmake -S libs/world -B build.world -DBLIT_WORLD_TESTS=ON
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
	cmake_minimum_required(VERSION 3.15...3.31)
	project(BlitWorld CXX)
	option(BLIT_WORLD_TESTS "Build the unit tests for the SDK world library" OFF)
endif()

add_library(BlitWorld STATIC
	actions.cpp
	registry.cpp
	scene.cpp
	scene_world.cpp
	world.cpp
)

target_include_directories(BlitWorld PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_features(BlitWorld PUBLIC cxx_std_17)

# No /Zc:preprocessor here on purpose. SDK_COMPONENT's SDK_EXPAND indirection
# makes it expand identically under MSVC's traditional and conformant
# preprocessors (checked with /P on both), and forcing the flag would change
# how every consumer's own code is preprocessed.

if(BLIT_WORLD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
