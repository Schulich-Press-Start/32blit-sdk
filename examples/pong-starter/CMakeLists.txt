cmake_minimum_required(VERSION 3.21)
project(pong_starter LANGUAGES C CXX)

find_package(32BLIT CONFIG REQUIRED PATHS
    "${CMAKE_CURRENT_SOURCE_DIR}/../32blit-sdk"
    "${CMAKE_CURRENT_SOURCE_DIR}/../..")
blit_executable(pong_starter pong.cpp)
set_target_properties(pong_starter PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
    set_target_properties(pong_starter PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
endif()
