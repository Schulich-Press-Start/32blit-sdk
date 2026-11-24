# Unit tests for sdk::world, built only with -DBLIT_WORLD_TESTS=ON.

include(FetchContent)

# GoogleTest, pinned to a release and checked against its hash.
FetchContent_Declare(googletest
	URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
	URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# keep gtest out of `install` and the release zips
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
# not needed yet; turn on when a test needs mocks
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
# match the runtime library MSVC uses for the rest of the build
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(blit_world_tests
	actions_test.cpp
	collision_query_test.cpp
	grid_navigation_test.cpp
	grid_route_test.cpp
	grid_step_test.cpp
	map_test.cpp
	overlap_tracker_test.cpp
	registry_test.cpp
	scene_test.cpp
	tile_movement_test.cpp
	tile_props_test.cpp
	value_test.cpp
	world_test.cpp
	../../../32blit/types/map.cpp
)
target_link_libraries(blit_world_tests PRIVATE BlitWorld GTest::gtest_main)
if(WIN32)
	target_compile_definitions(blit_world_tests PRIVATE WIN32)
endif()

include(GoogleTest)
gtest_discover_tests(blit_world_tests DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES)

# Its own program, not a GoogleTest case: it replaces global operator new and
# delete, which has to account for every allocation in the binary.
add_executable(blit_world_alloc_check no_allocation_check.cpp)
target_link_libraries(blit_world_alloc_check PRIVATE BlitWorld)
add_test(NAME NoAllocation.RuntimeWorldOperations COMMAND blit_world_alloc_check)
