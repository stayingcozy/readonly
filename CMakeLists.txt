cmake_minimum_required(VERSION 3.25)
project(readonly LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(READONLY_SANITIZE "Build with ASan+UBSan" OFF)
option(READONLY_WERROR "Treat warnings as errors" OFF)

# Shared compile posture: core/cli link this PRIVATE
add_library(readonly_options INTERFACE)
target_compile_features(readonly_options INTERFACE cxx_std_23)
target_compile_options(readonly_options INTERFACE 
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wsign-conversion>
    $<$<AND:$<BOOL:${READONLY_WERROR}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>
    $<$<AND:$<BOOL:${READONLY_SANITIZE}>,$<CXX_COMPILER_ID:GNU,Clang>>:-fsanitize=address,undefined;-fno-omit-frame-pointer>
)
target_link_options(readonly_options INTERFACE
    $<$<AND:$<BOOL:${READONLY_SANITIZE}>,$<CXX_COMPILER_ID:GNU,Clang>>:-fsanitize=address,undefined>
)
# libstdc++ bounds-checks in Debug. For libc++ swap to _LIBCPP_HARDENING_MODE
target_compile_definitions(readonly_options INTERFACE 
    $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
)

include(FetchContent)
FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.6.2
)
FetchContent_MakeAvailable(cli11)

add_subdirectory(shared)
add_subdirectory(core)
add_subdirectory(cli)
# add_subdirectory(gui)  # later links readonly_core + Raylib/(maybe)ClayUI
