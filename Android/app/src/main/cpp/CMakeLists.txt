cmake_minimum_required(VERSION 3.22)
project(solarnx-android C CXX)

# Shared UI library + its third_party deps (SDL2 shared; SDL_image/freetype/RmlUi static).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../Ui ${CMAKE_BINARY_DIR}/solarnx-ui)

# SDLActivity loads libSDL2.so + libmain.so; everything else is linked into main statically.
add_library(main SHARED android_main.cpp)
target_link_libraries(main PRIVATE solarnx-ui SDL2::SDL2 android log)
