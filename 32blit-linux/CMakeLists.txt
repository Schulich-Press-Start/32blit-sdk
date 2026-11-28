# 32blit-linux: port of the 32blit SDK to Linux without a window system or SDL.
#
# Selected by configuring with -D32BLIT_LINUX=ON (see 32blit-config.cmake).
# Builds the port into the BlitHalLinux static library and defines the two
# functions every port must provide for game CMakeLists files:
#   blit_executable(NAME sources...)  - build a game
#   blit_metadata(TARGET metadata.yml) - attach title/author/version to it

# On other systems (macOS) only the unit tests build: the port itself needs
# Linux's DRM, input and console APIs. Games are still defined, so a project's
# CMakeLists works unchanged, but left out of the build.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	if(NOT BLIT_LINUX_TESTS)
		message(FATAL_ERROR "32blit-linux games build only for Linux, not ${CMAKE_SYSTEM_NAME}.\n"
			"Desktop (SDL) build: configure without -D32BLIT_LINUX=ON.\n"
			"T113 build: configure with -DCMAKE_TOOLCHAIN_FILE=<32blit-sdk>/t113.toolchain instead.\n"
			"-DBLIT_LINUX_TESTS=ON builds just the port's unit tests here.\n")
	endif()

	function(blit_executable NAME)
		message(STATUS "Skipping ${NAME}: 32blit-linux games build only for Linux")
		blit_executable_args(${ARGN})
		add_executable(${NAME} EXCLUDE_FROM_ALL ${SOURCES})
	endfunction()

	function(blit_metadata TARGET FILE)
	endfunction()

	add_subdirectory(tests)
	return()
endif()

add_library(BlitHalLinux STATIC
	console.cpp
	default_metadata.cpp
	display.cpp
	display_drm.cpp
	display_fb.cpp
	file.cpp
	input_evdev.cpp
	main.cpp
	panel.cpp
)

target_link_libraries(BlitHalLinux PUBLIC BlitEngine)

# libdrm, for the DRM/KMS display (display_drm.cpp). PC builds use the distro's
# package; the T113 build uses the one tools/build-libdrm.sh installs into the
# toolchain's sysroot (t113.toolchain points pkg-config there).
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
	pkg_check_modules(LIBDRM IMPORTED_TARGET libdrm)
endif()
if(NOT LIBDRM_FOUND)
	message(FATAL_ERROR "libdrm not found.\n"
		"PC build: install it (Debian/Ubuntu: sudo apt install libdrm-dev pkg-config).\n"
		"T113 build: run 32blit-sdk/tools/build-libdrm.sh.\n")
endif()
target_link_libraries(BlitHalLinux PUBLIC PkgConfig::LIBDRM)

# Build a game executable linked against the port.
#
# The port library is linked with --whole-archive so the linker keeps
# main() and the blit::api tables even though the game doesn't reference
# them directly.
function(blit_executable NAME)
	message(STATUS "Processing ${NAME}")
	blit_executable_args(${ARGN})

	add_executable(${NAME} ${SOURCES})

	target_link_libraries(${NAME} -Wl,--whole-archive BlitHalLinux -Wl,--no-whole-archive)

	install(TARGETS ${NAME}
		RUNTIME DESTINATION bin
	)
endfunction()

# Read a game's metadata.yml (with the 32blit tools) and compile it into
# the game as the metadata_* strings declared in user_code.hpp. These
# replace the weak defaults from default_metadata.cpp.
function(blit_metadata TARGET FILE)
	if(NOT EXISTS ${FILE})
		set(FILE ${CMAKE_CURRENT_SOURCE_DIR}/${FILE})
	endif()

	# reconfigure whenever the metadata changes
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FILE})

	# parse the metadata into METADATA_* variables
	execute_process(
		COMMAND ${32BLIT_TOOLS_EXECUTABLE} cmake --config ${FILE} --cmake ${CMAKE_CURRENT_BINARY_DIR}/metadata.cmake
		RESULT_VARIABLE TOOL_RESULT
	)
	if(${TOOL_RESULT})
		message(FATAL_ERROR "Reading metadata config failed!\n")
	endif()

	include(${CMAKE_CURRENT_BINARY_DIR}/metadata.cmake)

	string(CONFIGURE "
const char *metadata_title = \"\${METADATA_TITLE}\";
const char *metadata_author = \"\${METADATA_AUTHOR}\";
const char *metadata_description = \"\${METADATA_DESCRIPTION}\";
const char *metadata_version = \"\${METADATA_VERSION}\";
const char *metadata_url = \"\${METADATA_URL}\";
const char *metadata_category = \"\${METADATA_CATEGORY}\";"
		METADATA_CONTENT ESCAPE_QUOTES
	)

	file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_metadata.cpp CONTENT "${METADATA_CONTENT}")

	target_sources(${TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_metadata.cpp)
endfunction()

# Unit tests, see tests/CMakeLists.txt (option BLIT_LINUX_TESTS is in 32blit-config.cmake).
if(BLIT_LINUX_TESTS)
	add_subdirectory(tests)
endif()
