# blit_scene_game(game SOURCES game.cpp [more.cpp ...])
function(blit_scene_game target)
    if(CMAKE_MINIMUM_REQUIRED_VERSION VERSION_LESS 3.21)
        message(FATAL_ERROR "blit_scene_game requires cmake_minimum_required(VERSION 3.21) or later")
    endif()
    if((NOT TARGET BlitHalSDL AND NOT TARGET BlitHalLinux) OR EMSCRIPTEN OR ANDROID)
        message(FATAL_ERROR "blit_scene_game requires a desktop SDL or native Linux filesystem build")
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 game "" "" "SOURCES")
    if(NOT game_SOURCES OR game_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Usage: blit_scene_game(target SOURCES game.cpp [more.cpp ...])")
    endif()

    set(scene_sdk "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    if(NOT TARGET BlitGameScene)
        add_subdirectory("${scene_sdk}" "${CMAKE_BINARY_DIR}/libs/game_scene" EXCLUDE_FROM_ALL)
    endif()
    if(BLIT_SCENE_PROJECT_DIR)
        file(REAL_PATH "${BLIT_SCENE_PROJECT_DIR}" project_root BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        set(project_root "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    if(NOT EXISTS "${project_root}/blit-project.json")
        message(FATAL_ERROR "Missing ${project_root}/blit-project.json (the scene project's root manifest)")
    endif()

    blit_executable(${target} ${game_SOURCES})
    target_link_libraries(${target} BlitGameScene)
    set_target_properties(${target} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${project_root}")
    if(WIN32)
        # Match the editor's path for multi-config VS and single-config Ninja.
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
    endif()

    # NOT POST_BUILD: pressing Build after editing/removing only art must sync
    # even when the executable itself is already up to date. CMP0112 NEW keeps
    # TARGET_FILE_DIR from implicitly depending on the executable (a cycle).
    add_custom_target(${target}_scene_data
        COMMAND "${CMAKE_COMMAND}" "-Dproject_root=${project_root}" "-Ddestination=$<TARGET_FILE_DIR:${target}>/data"
            -P "${scene_sdk}/StageScene.cmake"
        COMMENT "Synchronizing ${target} scene data" VERBATIM)
    add_dependencies(${target} ${target}_scene_data)

    set(install_data bin/data)
    if(APPLE)
        set(install_data "bin/$<TARGET_FILE_BASE_NAME:${target}>.app/Contents/MacOS/data")
    endif()
    # Read the source at install time, including changes since the last build.
    install(CODE "
        set(project_root [==[${project_root}]==])
        set(destination \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${install_data}\")
        include([==[${scene_sdk}/StageScene.cmake]==])")

    if(MSVC)
        # Only release-family MSVC runtimes may be redistributed. Debug builds
        # are for local development with the matching developer tools installed.
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
        include(InstallRequiredSystemLibraries)
        if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
            install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION bin
                CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
        endif()
    elseif(MINGW)
        # UCRT64 SDL can have transitive codec and compiler DLL dependencies.
        # Resolve the executable's actual imports, not a hard-coded DLL list.
        get_filename_component(compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" "-Dexecutable=$<TARGET_FILE:${target}>" "-Ddestination=$<TARGET_FILE_DIR:${target}>"
                "-Dcompiler_bin=${compiler_bin}" "-Dobjdump=${CMAKE_OBJDUMP}" -P "${scene_sdk}/StageMingwRuntime.cmake"
            VERBATIM)
        install(CODE "
            set(executable [==[$<TARGET_FILE:${target}>]==])
            set(compiler_bin [==[${compiler_bin}]==])
            set(objdump [==[${CMAKE_OBJDUMP}]==])
            set(destination \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin\")
            include([==[${scene_sdk}/StageMingwRuntime.cmake]==])")
    endif()
endfunction()
