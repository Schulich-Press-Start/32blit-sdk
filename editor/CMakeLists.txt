cmake_minimum_required(VERSION 3.21)
project(32blit-editor LANGUAGES CXX)

if(EMSCRIPTEN)
  enable_language(C)
  set(editor_target 32blit-editor-web)
else()
  set(editor_target 32blit-editor)
  if(WIN32)
    find_package(SDL2 2.24 CONFIG REQUIRED) # Windows per-monitor DPI hint
  else()
    find_package(SDL2 2.0.18 CONFIG REQUIRED)
  endif()
  find_package(Threads REQUIRED)
endif()

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)
FetchContent_Declare(imgui
  # v1.91.9b-docking
  URL https://codeload.github.com/ocornut/imgui/tar.gz/refs/tags/v1.91.9b-docking
  DOWNLOAD_NAME imgui.tar.gz
  URL_HASH SHA256=466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5
)
FetchContent_Declare(nlohmann_json
  # v3.12.0
  URL https://codeload.github.com/nlohmann/json/tar.gz/55f93686c01528224f448c19128836e7df245f72
  DOWNLOAD_NAME nlohmann-json.tar.gz
  URL_HASH SHA256=67f4cdd9ca930c9c1e130af4a437c7fc98fab77a2846fc2d2a14b4943831f8ef
)
FetchContent_Declare(stb
  URL https://codeload.github.com/nothings/stb/tar.gz/f0569113c93ad095470c54bf34a17b36646bbbb5
  URL_HASH SHA256=4d05c96640ae3a8cbdafdad8d344b50ce610802f78aee80154acdb8e266282e0
)
FetchContent_MakeAvailable(imgui nlohmann_json stb)

if(EMSCRIPTEN)
  FetchContent_Declare(miniz
    # 3.1.1
    URL https://codeload.github.com/richgel999/miniz/tar.gz/refs/tags/3.1.1
    DOWNLOAD_NAME miniz.tar.gz
    URL_HASH SHA256=8bb29c7bd6f22356e5583e794bed4a0b3e6dfcbcadb49974fc9270ccca1e5557
  )
  FetchContent_MakeAvailable(miniz)
elseif(TARGET SDL2::SDL2)
  set(editor_sdl SDL2::SDL2)
else()
  set(editor_sdl SDL2::SDL2-static)
endif()

add_subdirectory(../libs/world world)

add_executable(${editor_target}
  main.cpp
  project.cpp
  documents.cpp
  scene_canvas.cpp
  scene_preview.cpp
  preview_image.cpp
  tile_preview.cpp
  tileset_assets.cpp
  sprite_assets.cpp
  ../32blit/types/map.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_compile_features(${editor_target} PRIVATE cxx_std_17)
target_compile_definitions(${editor_target} PRIVATE SDL_MAIN_HANDLED)
target_include_directories(${editor_target} PRIVATE ${imgui_SOURCE_DIR} ${stb_SOURCE_DIR})
target_link_libraries(${editor_target} PRIVATE BlitWorld nlohmann_json::nlohmann_json)

if(EMSCRIPTEN)
  set(web_shell ${CMAKE_CURRENT_SOURCE_DIR}/web_shell.html)
  set(web_script ${CMAKE_CURRENT_SOURCE_DIR}/web_shell.js)
  set_target_properties(${editor_target} PROPERTIES SUFFIX ".html" LINK_DEPENDS "${web_shell};${web_script}")
  target_sources(${editor_target} PRIVATE web_project.cpp)
  target_compile_options(BlitWorld PRIVATE -fexceptions)
  target_compile_options(${editor_target} PRIVATE -sUSE_SDL=2 -fexceptions)
  target_link_libraries(${editor_target} PRIVATE miniz)
  target_link_options(${editor_target} PRIVATE
    -fexceptions -sUSE_SDL=2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576
    -sEXPORTED_RUNTIME_METHODS=ccall,FS
    "SHELL:--shell-file \"${web_shell}\"" "SHELL:--pre-js \"${web_script}\""
  )
else()
  target_sources(${editor_target} PRIVATE bridge_client.cpp process.cpp)
  target_link_libraries(${editor_target} PRIVATE ${editor_sdl} Threads::Threads)
endif()

if(WIN32)
  enable_language(RC)
  target_sources(32blit-editor PRIVATE editor.rc)
  target_link_libraries(32blit-editor PRIVATE ws2_32 comdlg32)
  get_target_property(editor_sdl_type ${editor_sdl} TYPE)
  if(editor_sdl_type STREQUAL "SHARED_LIBRARY")
    add_custom_command(TARGET 32blit-editor POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:${editor_sdl}> $<TARGET_FILE_DIR:32blit-editor>
      VERBATIM
    )
  endif()
endif()
