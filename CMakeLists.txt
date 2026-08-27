cmake_minimum_required(VERSION 3.16)

project(TorrentShopNX C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Platform defaults
if (NOT DEFINED PLATFORM_DESKTOP AND NOT DEFINED PLATFORM_SWITCH)
    set(PLATFORM_DESKTOP ON CACHE BOOL "Build for Desktop" FORCE)
endif ()

# Borealis library configuration
set(BOREALIS_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/_external/borealis/library")
set(USE_LIBROMFS OFF CACHE BOOL "Use LibRomFS" FORCE)
set(USE_SHARED_LIB OFF CACHE BOOL "Use shared libraries" FORCE)
set(USE_SYSTEM_FMT OFF CACHE BOOL "Use system fmt" FORCE)
set(USE_SYSTEM_TINYXML2 OFF CACHE BOOL "Use system tinyxml2" FORCE)
set(USE_SYSTEM_TWEENY OFF CACHE BOOL "Use system tweeny" FORCE)

if (WIN32 OR MINGW OR MSYS)
    set(GLFW_BUILD_WIN32 ON CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
endif ()

include("${BOREALIS_LIBRARY}/cmake/commonOption.cmake")
include("${BOREALIS_LIBRARY}/cmake/toolchain.cmake")

add_subdirectory("${BOREALIS_LIBRARY}" borealis_lib EXCLUDE_FROM_ALL)

if (TARGET glfw)
    target_compile_definitions(glfw PRIVATE _GLFW_WIN32 _WIN32)
endif ()

# Collect all sources from the source tree
file(GLOB_RECURSE APP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/source/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp"
)

add_executable(TorrentShopNX ${APP_SOURCES})
set_target_properties(TorrentShopNX PROPERTIES LINKER_LANGUAGE CXX)

# Include directories
target_include_directories(TorrentShopNX PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/engine"
    "${CMAKE_CURRENT_SOURCE_DIR}/source"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/catalog"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/download"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/installer"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/net"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/utils"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/datasource"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/buffer"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/config"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/engine"
    "${CMAKE_CURRENT_SOURCE_DIR}/pctest/deps/include"
    "${BOREALIS_LIBRARY}/include"
    "${BOREALIS_LIBRARY}/include/borealis/extern"
    "${BOREALIS_LIBRARY}/include/borealis/extern/nanovg"
    "${BOREALIS_LIBRARY}/lib/extern/fmt/include"
    "${BOREALIS_LIBRARY}/lib/extern/tweeny/include"
    "${BOREALIS_LIBRARY}/lib/extern/yoga"
)

# Compiler definitions
target_compile_definitions(TorrentShopNX PRIVATE
    TSNX_USE_CUSTOM_ENGINE=1
    POSIX=1
    YG_ENABLE_EVENTS=1
)

# Dependencies
target_link_directories(TorrentShopNX PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/pctest/deps/lib"
)

target_link_libraries(TorrentShopNX PRIVATE
    -Wl,--start-group
    borealis
    curl
    mbedtls
    mbedx509
    mbedcrypto
    z
    pthread
    stdc++
    -Wl,--end-group
)

# Copy resources to the build directory so Borealis finds XML, images, fonts, i18n
add_custom_command(TARGET TorrentShopNX POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:TorrentShopNX>/resources"
    COMMENT "Copying resources directory to build target folder..."
)
