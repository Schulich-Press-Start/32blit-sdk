add_library(BlitGameScene STATIC game_scene.cpp ui_screen.cpp audio_cues.cpp progress_store.cpp)
target_compile_features(BlitGameScene PUBLIC cxx_std_17)
target_include_directories(BlitGameScene PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(TARGET BlitHalLinux)
    target_link_libraries(BlitGameScene PUBLIC BlitWorld BlitHalLinux)
else()
    target_link_libraries(BlitGameScene PUBLIC BlitWorld BlitHalSDL)
    if(NOT EMSCRIPTEN AND NOT ANDROID)
        target_compile_definitions(BlitGameScene PRIVATE BLIT_SCENE_SDL_AUDIO)
        target_include_directories(BlitGameScene PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../32blit-sdl")
    endif()
endif()

# Distributed SDKs provide these single headers. A source checkout uses the
# same pinned releases as the editor (or local FETCHCONTENT_SOURCE_DIR_*).
set(vendor "${CMAKE_CURRENT_SOURCE_DIR}/../../3rd-party")
target_include_directories(BlitGameScene PRIVATE "${vendor}")
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
if(NOT EXISTS "${vendor}/nlohmann/json.hpp")
    FetchContent_Declare(nlohmann_json
        URL https://codeload.github.com/nlohmann/json/tar.gz/55f93686c01528224f448c19128836e7df245f72
        DOWNLOAD_NAME nlohmann-json.tar.gz
        URL_HASH SHA256=67f4cdd9ca930c9c1e130af4a437c7fc98fab77a2846fc2d2a14b4943831f8ef)
    FetchContent_MakeAvailable(nlohmann_json)
    target_link_libraries(BlitGameScene PRIVATE nlohmann_json::nlohmann_json)
endif()
if(NOT EXISTS "${vendor}/stb_image.h")
    FetchContent_Declare(stb
        URL https://codeload.github.com/nothings/stb/tar.gz/f0569113c93ad095470c54bf34a17b36646bbbb5
        URL_HASH SHA256=4d05c96640ae3a8cbdafdad8d344b50ce610802f78aee80154acdb8e266282e0)
    FetchContent_MakeAvailable(stb)
    target_include_directories(BlitGameScene PRIVATE "${stb_SOURCE_DIR}")
endif()
