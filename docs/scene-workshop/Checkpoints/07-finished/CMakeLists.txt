cmake_minimum_required(VERSION 3.21)
project(game LANGUAGES C CXX)

find_package(32BLIT CONFIG REQUIRED COMPONENTS Scene
    HINTS ENV 32BLIT_DIR
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../32blit-sdk" "${CMAKE_CURRENT_SOURCE_DIR}/../..")

blit_scene_game(game SOURCES game.cpp paddle.cpp)

# The editor opens this schema before it knows anything about game-specific C++ types.
if(NOT CMAKE_CROSSCOMPILING)
    blit_component_metadata(game_registry "${CMAKE_CURRENT_SOURCE_DIR}/registry.json" paddle.cpp)
    add_dependencies(game game_registry)
endif()
