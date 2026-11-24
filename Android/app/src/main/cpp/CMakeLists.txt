cmake_minimum_required(VERSION 3.22)
project(solarnx-android C CXX)

# Shared UI library + its third_party deps (SDL2 shared; SDL_image/freetype/RmlUi static).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../Ui ${CMAKE_BINARY_DIR}/solarnx-ui)

# Portable session core (protocol, route detection, library channel). Tests are a
# desktop-only target - off for the device build.
set(SNX_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../Core ${CMAKE_BINARY_DIR}/solarnx-core)

# SDLActivity loads libSDL2.so + libmain.so; everything else is linked into main statically.
add_library(main SHARED android_main.cpp)
target_link_libraries(main PRIVATE solarnx-ui solarnx-core SDL2::SDL2 android log)

# Device-test tool: a synthetic SolarNX video host (see Core/tests/video_send.c).
# Built into the native build dir - push to /data/local/tmp and run from adb shell
# against the app's loopback listener when the PC can't reach the phone directly.
add_executable(video_send ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../Core/tests/video_send.c)
target_include_directories(video_send PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../Core/include
                                              ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../Core/src)
target_link_libraries(video_send solarnx-core)

# Same for the audio host tool.
add_executable(audio_send ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../Core/tests/audio_send.c)
target_include_directories(audio_send PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../Core/include)
target_link_libraries(audio_send solarnx-core)
