cmake_minimum_required(VERSION 3.21)
project(tower_defense LANGUAGES C CXX)

find_package(32BLIT CONFIG REQUIRED COMPONENTS Scene
    HINTS ENV 32BLIT_DIR PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../..")
blit_scene_game(game SOURCES game.cpp)

if(NOT CMAKE_CROSSCOMPILING)
    blit_component_metadata(tower_registry "${CMAKE_CURRENT_SOURCE_DIR}/registry.json")
    add_dependencies(game tower_registry)
    add_dependencies(game_scene_data tower_registry)
endif()
