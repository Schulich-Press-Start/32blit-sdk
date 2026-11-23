if (NOT DEFINED BLIT_ONCE)
	set(BLIT_ONCE TRUE)

	set(CMAKE_CXX_STANDARD 17)
	set(CMAKE_CXX_EXTENSIONS OFF)
	# Scene projects read authored files directly. Keep the legacy package's
	# tool discovery for existing callers, and for any packer used explicitly.
	macro(_blit_require_tools)
		if(NOT _blit_tools_checked)
			find_package(Python 3.6 REQUIRED COMPONENTS Interpreter)
			execute_process(COMMAND "${Python_EXECUTABLE}" -m ttblit version RESULT_VARIABLE tool_status OUTPUT_VARIABLE tool_version ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
			if(tool_status)
				find_program(32BLIT_TOOLS_EXECUTABLE 32blit)
				if(32BLIT_TOOLS_EXECUTABLE)
					execute_process(COMMAND ${32BLIT_TOOLS_EXECUTABLE} version RESULT_VARIABLE tool_status OUTPUT_VARIABLE tool_version ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
				endif()
			else()
				set(32BLIT_TOOLS_EXECUTABLE "${Python_EXECUTABLE}" -m ttblit)
			endif()
			if(tool_status OR tool_version VERSION_LESS 0.7.3)
				message(FATAL_ERROR "32blit tools >= 0.7.3 required. Install with: \"${Python_EXECUTABLE}\" -m pip install --upgrade 32blit \"bitstring<5\"")
			endif()
			set(_blit_tools_checked TRUE)
		endif()
	endmacro()
	if(NOT "Scene" IN_LIST 32BLIT_FIND_COMPONENTS OR 32BLIT_HW OR PICO_SDK_PATH)
		_blit_require_tools()
	endif()

	if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
		set(CMAKE_BUILD_TYPE "Release")
	endif()

	if(WIN32)
		add_definitions("-DWIN32")
	endif()

	add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/3rd-party 3rd-party)
	add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/32blit 32blit)

	option(BLIT_WORLD_TESTS "Build the unit tests for the SDK world library" OFF)
	if(BLIT_WORLD_TESTS)
		enable_testing()
		add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/libs/world libs/world)
	else()
		add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/libs/world libs/world EXCLUDE_FROM_ALL)
	endif()
	include("${CMAKE_CURRENT_LIST_DIR}/tools/blit-tools/Tiled.cmake")

	function (blit_assets_yaml TARGET FILE)
		_blit_require_tools()
		# cause cmake to reconfigure whenever the asset list changes
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${FILE})

		# get the inputs/outputs for the asset tool (at configure time)
		execute_process(
		    COMMAND ${32BLIT_TOOLS_EXECUTABLE} --debug cmake --config ${CMAKE_CURRENT_SOURCE_DIR}/${FILE} --output ${CMAKE_CURRENT_BINARY_DIR} --cmake ${CMAKE_CURRENT_BINARY_DIR}/assets.cmake
            RESULT_VARIABLE TOOL_RESULT
		)
		if(${TOOL_RESULT})
		    message(FATAL_ERROR "Reading asset config failed!\n")
		endif()

		include(${CMAKE_CURRENT_BINARY_DIR}/assets.cmake)

		# do asset packing (at build time)
		add_custom_command(
			OUTPUT ${ASSET_OUTPUTS}
			COMMAND cd ${CMAKE_CURRENT_SOURCE_DIR} && ${32BLIT_TOOLS_EXECUTABLE} --debug  pack --force --config ${CMAKE_CURRENT_SOURCE_DIR}/${FILE} --output ${CMAKE_CURRENT_BINARY_DIR}
			DEPENDS ${ASSET_DEPENDS} ${CMAKE_CURRENT_SOURCE_DIR}/${FILE}
			VERBATIM
		)

		# add the outputs as dependencies of the project (also compile any cpp files)
		target_sources(${TARGET} PRIVATE ${ASSET_OUTPUTS})
		target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
	endfunction()

	# common helpers
	function(blit_executable_args)
		set(SOURCES)

		# enable one of the pico builds if neither set
		if(NOT BLIT_EXECUTABLE_PICO_STANDALONE_UF2 AND NOT BLIT_EXECUTABLE_PICO_BLIT)
			set(BLIT_EXECUTABLE_PICO_STANDALONE_UF2 TRUE)
		endif()

		# global overrides
		if(DEFINED BLIT_EXECUTABLE_INTERNAL_FLASH)
			set(INTERNAL_FLASH ${BLIT_EXECUTABLE_INTERNAL_FLASH})
		else()
			set(INTERNAL_FLASH FALSE)
		endif()

		if(DEFINED BLIT_EXECUTABLE_PICO_STANDALONE_UF2)
			set(PICO_STANDALONE_UF2 ${BLIT_EXECUTABLE_PICO_STANDALONE_UF2})
		else()
			set(PICO_STANDALONE_UF2 FALSE)
		endif()

		if(DEFINED BLIT_EXECUTABLE_PICO_BLIT)
			set(PICO_BLIT ${BLIT_EXECUTABLE_PICO_BLIT})
		else()
			set(PICO_BLIT FALSE)
		endif()

		if(DEFINED BLIT_EXECUTABLE_PICO_BLIT_OFFSET_KB)
			set(PICO_BLIT_OFFSET_KB ${BLIT_EXECUTABLE_PICO_BLIT_OFFSET_KB})
		endif()

		foreach(arg IN LISTS ARGN)
			if(arg STREQUAL "INTERNAL_FLASH")
				set(${arg} TRUE)
			elseif(arg STREQUAL "PICO_STANDALONE_UF2")
				set(${arg} TRUE)
				set(PICO_BLIT FALSE) # can't build both
			elseif(arg STREQUAL "PICO_BLIT")
				set(${arg} TRUE)
				set(PICO_STANDALONE_UF2 FALSE)
			# args with value
			elseif(arg STREQUAL "PICO_BLIT_OFFSET_KB")
				set(prev_arg ${arg})
			# value for args with one
			elseif(prev_arg STREQUAL "PICO_BLIT_OFFSET_KB")
				set(${prev_arg} ${arg})
				unset(prev_arg)
			else()
				list(APPEND SOURCES ${arg})
			endif()
		endforeach()

		set(SOURCES ${SOURCES} PARENT_SCOPE)
		set(INTERNAL_FLASH ${INTERNAL_FLASH} PARENT_SCOPE)
		set(PICO_STANDALONE_UF2 ${PICO_STANDALONE_UF2} PARENT_SCOPE)
		set(PICO_BLIT ${PICO_BLIT} PARENT_SCOPE)
		set(PICO_BLIT_OFFSET_KB ${PICO_BLIT_OFFSET_KB} PARENT_SCOPE)
	endfunction()

	if (32BLIT_HW)
		set(FLASH_PORT "AUTO" CACHE STRING "Port to use for flash")

		include(${CMAKE_CURRENT_LIST_DIR}/32blit-stm32/executable.cmake)
	elseif(32BLIT_LINUX)
		# Unit tests for the port (PC builds only). Enabled here, in the top-level
		# directory, because ctest only finds tests when the top level enables testing.
		option(BLIT_LINUX_TESTS "Build the 32blit-linux unit tests" OFF)
		if(BLIT_LINUX_TESTS)
			enable_testing()
		endif()

		add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/32blit-linux 32blit-linux)
	elseif(PICO_SDK_PATH)
		include(${CMAKE_CURRENT_LIST_DIR}/32blit-pico/executable.cmake)
	else()
		add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/32blit-sdl 32blit-sdl)
	endif()
	include("${CMAKE_CURRENT_LIST_DIR}/libs/game_scene/SceneGame.cmake")
endif ()
