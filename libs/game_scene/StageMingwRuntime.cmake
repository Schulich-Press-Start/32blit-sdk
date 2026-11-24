cmake_minimum_required(VERSION 3.21)

# In cmake -P there is no configured compiler, so pass in its matching tool.
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${objdump}")
# System DLLs are supplied by Windows, not by the application. Exclude them
# before lookup so a Linux-hosted cross-build does not require System32.
# CMake lowercases PE dependency names when matching these filters.
set(system_dlls "^(advapi32|bcrypt|cfgmgr32|comdlg32|crypt32|dinput8|dwmapi|gdi32|hid|imm32|iphlpapi|kernel32|kernelbase|msimg32|msvcrt|ntdll|ole32|oleaut32|powrprof|secur32|setupapi|shell32|shlwapi|ucrtbase|user32|uxtheme|version|winmm|winspool|ws2_32)\\.dll$")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${executable}"
    DIRECTORIES "${compiler_bin}" "${destination}"
    RESOLVED_DEPENDENCIES_VAR dlls
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-" "${system_dlls}"
    POST_EXCLUDE_REGEXES "[/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\]" "[/\\\\][Ss][Yy][Ss][Ww][Oo][Ww]64[/\\\\]")
file(MAKE_DIRECTORY "${destination}")
foreach(dll IN LISTS dlls)
    get_filename_component(name "${dll}" NAME)
    if(NOT dll STREQUAL "${destination}/${name}")
        file(COPY_FILE "${dll}" "${destination}/${name}" ONLY_IF_DIFFERENT)
    endif()
endforeach()
