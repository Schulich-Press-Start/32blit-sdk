# The metadata executable links only the game's registration translation units.
# For a cross-build, export the manifest with a separate native build first.
function(blit_component_metadata TARGET OUTPUT)
	if(CMAKE_CROSSCOMPILING)
		message(FATAL_ERROR "blit_component_metadata requires a native build; pass its JSON to blit_tiled_assets for cross builds")
	endif()
	get_filename_component(output "${OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
	get_filename_component(output_dir "${output}" DIRECTORY)
	add_executable(${TARGET}_export EXCLUDE_FROM_ALL "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/export_registry.cpp" ${ARGN})
	target_link_libraries(${TARGET}_export PRIVATE BlitWorld)
	add_custom_command(OUTPUT "${output}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
		COMMAND ${TARGET}_export "${output}.tmp"
		COMMAND ${CMAKE_COMMAND} -E rename "${output}.tmp" "${output}"
		DEPENDS ${TARGET}_export VERBATIM)
	add_custom_target(${TARGET} DEPENDS "${output}")
endfunction()

function(blit_tiled_assets TARGET MAP SCHEMA REGISTRY)
	if(CMAKE_VERSION VERSION_LESS 3.21)
		message(FATAL_ERROR "blit_tiled_assets requires CMake 3.21+ for tileset dependency tracking")
	endif()
	find_package(Python 3.9 REQUIRED COMPONENTS Interpreter)
	set(tools "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
	get_filename_component(source "${MAP}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
	get_filename_component(schema "${SCHEMA}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
	get_filename_component(registry "${REGISTRY}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
	get_target_property(previous_schema ${TARGET} BLIT_TILE_SCHEMA)
	if(previous_schema AND NOT previous_schema STREQUAL schema)
		message(FATAL_ERROR "All Tiled maps for ${TARGET} must share one tile flag schema")
	endif()
	if(NOT previous_schema)
		set_property(TARGET ${TARGET} PROPERTY BLIT_TILE_SCHEMA "${schema}")
		set(headers "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_tiled")
		set(flags "${headers}/tile_flags.hpp")
		set(flags_stamp "${headers}/tile_flags.stamp")
		add_custom_command(OUTPUT "${flags_stamp}" "${flags}"
			COMMAND "${Python_EXECUTABLE}" -B "${tools}/blit_tools.py" tile-flags "${schema}" "${flags}"
			COMMAND ${CMAKE_COMMAND} -E touch "${flags_stamp}"
			DEPENDS "${schema}" "${tools}/blit_tools.py" "${tools}/blit_tiled.py" VERBATIM)
		add_custom_target(${TARGET}_tile_flags DEPENDS "${flags_stamp}" "${flags}")
		add_dependencies(${TARGET} ${TARGET}_tile_flags)
		target_include_directories(${TARGET} PRIVATE "${headers}")
	endif()
	string(REGEX REPLACE "\\.tmx$" "" stem "${MAP}")
	set(scene "${CMAKE_CURRENT_SOURCE_DIR}/generated/${stem}.generated.scene")
	string(SHA256 id "${MAP}")
	set(depfile "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-${id}.d")
	set(stamp "${depfile}.stamp")
	add_custom_command(OUTPUT "${stamp}" "${scene}"
		COMMAND "${Python_EXECUTABLE}" -B "${tools}/blit_tools.py" import "${MAP}"
			--project-dir "${CMAKE_CURRENT_SOURCE_DIR}" --schema "${schema}" --registry "${registry}"
			--depfile "${depfile}" --dep-target "${stamp}"
		COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
		DEPENDS "${source}" "${schema}" "${registry}" "${tools}/blit_tools.py" "${tools}/blit_tiled.py"
		DEPFILE "${depfile}" VERBATIM)
	add_custom_target(${TARGET}_tiled_${id} DEPENDS "${stamp}" "${scene}")
	add_dependencies(${TARGET} ${TARGET}_tiled_${id})
endfunction()
