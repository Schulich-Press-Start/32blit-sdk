cmake_minimum_required(VERSION 3.21)
project(team_pong LANGUAGES C CXX)

# A copied sample can sit next to 32blit-sdk; the checked-in sample lives inside it.
find_package(32BLIT CONFIG REQUIRED PATHS
    "${CMAKE_CURRENT_SOURCE_DIR}/../32blit-sdk"
    "${CMAKE_CURRENT_SOURCE_DIR}/../..")

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://codeload.github.com/nlohmann/json/tar.gz/55f93686c01528224f448c19128836e7df245f72
    DOWNLOAD_NAME nlohmann-json.tar.gz
    URL_HASH SHA256=67f4cdd9ca930c9c1e130af4a437c7fc98fab77a2846fc2d2a14b4943831f8ef)
FetchContent_MakeAvailable(nlohmann_json)

blit_executable(team_pong pong.cpp paddle.cpp)
blit_assets_yaml(team_pong assets.yml)
blit_metadata(team_pong metadata.yml)
target_link_libraries(team_pong BlitWorld nlohmann_json::nlohmann_json)
set_target_properties(team_pong PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
    set_target_properties(team_pong PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
endif()

blit_component_metadata(team_pong_registry registry.json paddle.cpp)
add_dependencies(team_pong team_pong_registry)
