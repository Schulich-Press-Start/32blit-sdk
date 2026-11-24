cmake_minimum_required(VERSION 3.21)
project(team_pong LANGUAGES C CXX)

# A copied sample can sit next to 32blit-sdk; the checked-in sample lives inside it.
find_package(32BLIT CONFIG REQUIRED COMPONENTS Scene
    HINTS ENV 32BLIT_DIR
    PATHS
    "${CMAKE_CURRENT_SOURCE_DIR}/../32blit-sdk"
    "${CMAKE_CURRENT_SOURCE_DIR}/../..")

blit_scene_game(team_pong SOURCES pong.cpp paddle.cpp)

# Registry JSON is for the host editor; the target already links the component.
if(NOT CMAKE_CROSSCOMPILING)
    blit_component_metadata(team_pong_registry registry.json paddle.cpp)
    add_dependencies(team_pong team_pong_registry)
endif()
