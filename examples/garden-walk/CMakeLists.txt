cmake_minimum_required(VERSION 3.21)
project(garden_walk LANGUAGES C CXX)

find_package(32BLIT CONFIG REQUIRED COMPONENTS Scene
    HINTS ENV 32BLIT_DIR PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../..")
blit_scene_game(game SOURCES game.cpp door.cpp)
if(TARGET BlitHalSDL AND NOT EMSCRIPTEN AND NOT ANDROID)
    target_compile_definitions(game PRIVATE GARDEN_ACTION_BINDINGS)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    blit_component_metadata(garden_registry registry.json door.cpp)
    add_dependencies(game garden_registry)
endif()
