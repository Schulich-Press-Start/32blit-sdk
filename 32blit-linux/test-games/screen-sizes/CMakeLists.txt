cmake_minimum_required(VERSION 3.9...3.26)
project(screen-sizes)

# Test game for screen sizes (port plan step 6): builds for the desktop (SDL),
# 32blit-linux (-D32BLIT_LINUX=ON) and the T113 (t113.toolchain).
find_package(32BLIT CONFIG REQUIRED PATHS ../../..)

blit_executable(screen-sizes screen-sizes.cpp)
blit_metadata(screen-sizes metadata.yml)
