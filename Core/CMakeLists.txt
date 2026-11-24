# libsolarnx-core: the portable SolarNX session core (C99, zero dependencies).
# Part of SolarNX (GPL-3.0).
cmake_minimum_required(VERSION 3.16)
project(solarnx-core C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(solarnx-core STATIC
    src/core.c
    src/input.c
    src/route.c
    src/library.c
    src/net.c
    src/video.c
    src/audio.c
    src/rs.c
    src/backend_solar.c
    src/backend_sunshine.c
)
target_include_directories(solarnx-core PUBLIC include PRIVATE src)

option(SNX_CORE_BUILD_TESTS "Build the solarnx-core unit tests" ON)
if(SNX_CORE_BUILD_TESTS)
    enable_testing()
    add_executable(snx-core-tests
        tests/test_main.c
        tests/test_wire.c
        tests/test_route.c
        tests/test_library.c
        tests/test_video.c
        tests/test_audio.c
    )
    target_include_directories(snx-core-tests PRIVATE include src tests)
    target_link_libraries(snx-core-tests solarnx-core)
    add_test(NAME core COMMAND snx-core-tests)

    # Synthetic hosts for device tests - see their usage messages.
    add_executable(video_send tests/video_send.c)
    target_include_directories(video_send PRIVATE src)
    target_link_libraries(video_send solarnx-core)
    add_executable(audio_send tests/audio_send.c)
    target_link_libraries(audio_send solarnx-core)
endif()
