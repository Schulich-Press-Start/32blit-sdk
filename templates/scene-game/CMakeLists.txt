cmake_minimum_required(VERSION 3.21)
project(scene_game LANGUAGES C CXX)

find_package(32BLIT CONFIG REQUIRED COMPONENTS Scene
    HINTS ENV 32BLIT_DIR
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../32blit-sdk" "${CMAKE_CURRENT_SOURCE_DIR}/../..")

blit_scene_game(game SOURCES game.cpp)
