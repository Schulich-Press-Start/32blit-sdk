# Shared by World, the scene runtime, and the editor that authors its assets.
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

FetchContent_Declare(nlohmann_json
    # v3.12.0
    URL https://codeload.github.com/nlohmann/json/tar.gz/55f93686c01528224f448c19128836e7df245f72
    DOWNLOAD_NAME nlohmann-json.tar.gz
    URL_HASH SHA256=67f4cdd9ca930c9c1e130af4a437c7fc98fab77a2846fc2d2a14b4943831f8ef)
FetchContent_Declare(stb
    URL https://codeload.github.com/nothings/stb/tar.gz/f0569113c93ad095470c54bf34a17b36646bbbb5
    URL_HASH SHA256=4d05c96640ae3a8cbdafdad8d344b50ce610802f78aee80154acdb8e266282e0)
FetchContent_Declare(fast_float
    # v8.4.0
    URL https://codeload.github.com/fastfloat/fast_float/tar.gz/v8.4.0
    URL_HASH SHA256=0bf4277827259720028bbef6c11d400f07482608bea5952b289baaef6af141a4)
