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
