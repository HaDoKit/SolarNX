cmake_minimum_required(VERSION 3.20)

# Standalone configure (desktop) vs add_subdirectory (Android gradle build).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(solarnx-ui C CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SNXUI_BUILD_HOST "Build the desktop preview host" ON)
if(ANDROID)
    set(SNXUI_BUILD_HOST OFF)
endif()

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# All support libs are static. SDL is the exception on Android: SDLActivity
# calls System.loadLibrary("SDL2"), so it must ship as libSDL2.so.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# --- SDL2 ---
set(SDL_TEST OFF CACHE BOOL "" FORCE)
if(ANDROID)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(${TP}/SDL ${CMAKE_BINARY_DIR}/third_party/SDL EXCLUDE_FROM_ALL)

# SDL_image picks SDL2::SDL2-static for static builds; on Android SDL itself is
# shared, so satisfy the name with an alias to the shared target.
if(ANDROID AND TARGET SDL2 AND NOT TARGET SDL2::SDL2-static)
    add_library(SDL2::SDL2-static ALIAS SDL2)
endif()

# --- SDL2_image (stb backend covers PNG/JPG - no codec deps needed) ---
set(SDL2IMAGE_VENDORED OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_WEBP OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${TP}/SDL_image ${CMAKE_BINARY_DIR}/third_party/SDL_image EXCLUDE_FROM_ALL)

# --- FreeType (for RmlUi's default font engine) ---
set(FT_DISABLE_ZLIB TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)
add_subdirectory(${TP}/freetype ${CMAKE_BINARY_DIR}/third_party/freetype EXCLUDE_FROM_ALL)
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# --- RmlUi ---
# Keep find_package(Freetype) out: our vendored target already provides Freetype::Freetype.
set(CMAKE_DISABLE_FIND_PACKAGE_Freetype TRUE)
set(RMLUI_FONT_ENGINE freetype CACHE STRING "" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${TP}/RmlUi ${CMAKE_BINARY_DIR}/third_party/RmlUi EXCLUDE_FROM_ALL)

# --- libsolarnx-ui ---
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../VERSION SNXUI_VERSION LIMIT_COUNT 1)

add_library(solarnx-ui STATIC
    src/shell.cpp
    src/strings.cpp
    src/starfield.cpp
    src/file_interface.cpp
    ${TP}/RmlUi/Backends/RmlUi_Platform_SDL.cpp
    # Vendored copy: upstream rejects GLES renderers, Android needs GLES2 (SDL_VIDEO_RENDER_OGL_ES*).
    src/backends/RmlUi_Renderer_SDL.cpp
)
target_include_directories(solarnx-ui
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${TP}/RmlUi/Include ${TP}/RmlUi/Backends
)
target_compile_definitions(solarnx-ui PRIVATE
    RMLUI_SDL_VERSION_MAJOR=2
    SOLARNX_UI_VERSION="${SNXUI_VERSION}"
)
# SDL2_image exports SDL2_image::SDL2_image (shared) or ::SDL2_image-static.
if(TARGET SDL2_image::SDL2_image)
    set(SNXUI_IMG_TARGET SDL2_image::SDL2_image)
else()
    set(SNXUI_IMG_TARGET SDL2_image::SDL2_image-static)
endif()
target_link_libraries(solarnx-ui PUBLIC SDL2::SDL2 ${SNXUI_IMG_TARGET} rmlui_core)

if(SNXUI_BUILD_HOST)
    add_executable(snxui-host host/main.cpp)
    target_link_libraries(snxui-host PRIVATE solarnx-ui)
endif()
