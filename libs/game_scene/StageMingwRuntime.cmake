cmake_minimum_required(VERSION 3.21)

# In cmake -P there is no configured compiler, so pass in its matching tool.
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${objdump}")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${executable}"
    DIRECTORIES "${compiler_bin}"
    RESOLVED_DEPENDENCIES_VAR dlls
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "[/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\]" "[/\\\\][Ss][Yy][Ss][Ww][Oo][Ww]64[/\\\\]")
file(MAKE_DIRECTORY "${destination}")
foreach(dll IN LISTS dlls)
    get_filename_component(name "${dll}" NAME)
    if(NOT dll STREQUAL "${destination}/${name}")
        file(COPY_FILE "${dll}" "${destination}/${name}" ONLY_IF_DIFFERENT)
    endif()
endforeach()
