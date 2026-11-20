cmake_minimum_required(VERSION 3.21)
project(32blit-editor LANGUAGES CXX)

find_package(SDL2 2.0.18 CONFIG REQUIRED)
find_package(Threads REQUIRED)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)
FetchContent_Declare(imgui
  # v1.91.9b
  URL https://codeload.github.com/ocornut/imgui/tar.gz/f5befd2d29e66809cd1110a152e375a7f1981f06
  DOWNLOAD_NAME imgui.tar.gz
  URL_HASH SHA256=85f4ce357df05bcc331b587f01976f47fb55f19fadf477a7907289686bc3f4c8
)
FetchContent_Declare(nlohmann_json
  # v3.12.0
  URL https://codeload.github.com/nlohmann/json/tar.gz/55f93686c01528224f448c19128836e7df245f72
  DOWNLOAD_NAME nlohmann-json.tar.gz
  URL_HASH SHA256=67f4cdd9ca930c9c1e130af4a437c7fc98fab77a2846fc2d2a14b4943831f8ef
)
FetchContent_MakeAvailable(imgui nlohmann_json)

if(TARGET SDL2::SDL2)
  set(editor_sdl SDL2::SDL2)
else()
  set(editor_sdl SDL2::SDL2-static)
endif()

add_executable(32blit-editor
  main.cpp
  bridge_client.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_compile_features(32blit-editor PRIVATE cxx_std_17)
target_compile_definitions(32blit-editor PRIVATE SDL_MAIN_HANDLED)
target_include_directories(32blit-editor PRIVATE ${imgui_SOURCE_DIR})
target_link_libraries(32blit-editor PRIVATE ${editor_sdl} Threads::Threads nlohmann_json::nlohmann_json)

if(WIN32)
  target_link_libraries(32blit-editor PRIVATE ws2_32)
  get_target_property(editor_sdl_type ${editor_sdl} TYPE)
  if(editor_sdl_type STREQUAL "SHARED_LIBRARY")
    add_custom_command(TARGET 32blit-editor POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:${editor_sdl}> $<TARGET_FILE_DIR:32blit-editor>
      VERBATIM
    )
  endif()
endif()
