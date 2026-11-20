# Unit tests for 32blit-linux.
#
# Built only when configured with -DBLIT_LINUX_TESTS=ON (see 32blit-config.cmake).
#
#   cmake -S . -B build -D32BLIT_LINUX=ON -DBLIT_LINUX_TESTS=ON
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# With -DCMAKE_TOOLCHAIN_FILE=t113.toolchain instead of -D32BLIT_LINUX=ON, the
# tests build as ARM code and ctest runs them through qemu-arm.

include(FetchContent)

# GoogleTest, pinned to a release and checked against its hash.
FetchContent_Declare(googletest
	URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
	URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# keep gtest out of `install` and the release zips
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
# not needed yet; turn on when a test needs mocks
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# One test executable for the whole port.
# Tests compile the port's .cpp files they need directly, instead of linking
# BlitHalLinux, because that library contains the game's main().
add_executable(blit_linux_tests
	drm_select_test.cpp
	file_test.cpp
	panel_test.cpp
	screen_fit_test.cpp
	../file.cpp
	../panel.cpp
)
target_include_directories(blit_linux_tests PRIVATE ..)
target_link_libraries(blit_linux_tests PRIVATE BlitEngine GTest::gtest_main)

# Register every TEST() with CTest as its own test.
# PRE_TEST lists the tests when ctest runs, not during the build, so the build
# stays fast and still works when cross-compiling. NO_PRETTY_VALUES keeps the
# readable names from the tests' name generators (e.g. RGB565_to_RGB666).
include(GoogleTest)
gtest_discover_tests(blit_linux_tests DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES)
