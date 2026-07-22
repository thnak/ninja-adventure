cmake_minimum_required(VERSION 3.24)
project(QuarkMmo LANGUAGES CXX VERSION 0.1.0
        DESCRIPTION "Serverless farm-survival MMO on the Quark actor engine")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- Quark engine ----------------------------------------------------------
# Consumed as a source subproject. Tests/benches off — this build gates the GAME, not the engine
# (QuarkCpp's own suite is its gate). QUARK_DIR can point elsewhere for CI.
set(QUARK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../QuarkCpp" CACHE PATH "Path to the QuarkCpp checkout")
if(NOT EXISTS "${QUARK_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "QuarkCpp not found at '${QUARK_DIR}'. Set -DQUARK_DIR=/path/to/QuarkCpp.")
endif()

set(QUARK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QUARK_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(QUARK_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${QUARK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/quark" EXCLUDE_FROM_ALL)

# ---- Shared simulation -----------------------------------------------------
# Header-only: the world lives entirely in templates + inline code, like the engine it sits on.
add_library(mmo_world INTERFACE)
target_include_directories(mmo_world INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(mmo_world INTERFACE quark::quark)
target_compile_features(mmo_world INTERFACE cxx_std_23)

find_package(Threads REQUIRED)

# ---- Headless runner (no display required) ---------------------------------
add_executable(mmo_sim src/sim_main.cpp)
target_link_libraries(mmo_sim PRIVATE mmo_world Threads::Threads)

enable_testing()
add_test(NAME mmo_sim_smoke COMMAND mmo_sim 600)

# ---- Graphical client ------------------------------------------------------
# OFF by default so the simulation builds on a headless machine (and in CI) with no GL/X11 stack.
option(MMO_BUILD_CLIENT "Build the raylib graphical client" OFF)

if(MMO_BUILD_CLIENT)
  include(FetchContent)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(raylib)

  # raygui: single-header immediate-mode UI by the raylib author. Header-only, so it is fetched but
  # not built — one TU in src/ui/ defines RAYGUI_IMPLEMENTATION.
  FetchContent_Declare(raygui
    GIT_REPOSITORY https://github.com/raysan5/raygui.git
    GIT_TAG 4.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(raygui)

  add_executable(mmo_client
    src/client_main.cpp
    src/render/raylib_bridge.cpp
    src/ui/screens.cpp
    src/ui/audio.cpp)
  target_include_directories(mmo_client PRIVATE "${raygui_SOURCE_DIR}/src")
  target_link_libraries(mmo_client PRIVATE mmo_world raylib Threads::Threads)

  # World map exporter. Needs raylib only for ExportImage — it opens no window and touches no actor,
  # because terrain is a pure function. See src/worldmap_main.cpp.
  add_executable(mmo_worldmap src/worldmap_main.cpp)
  target_link_libraries(mmo_worldmap PRIVATE mmo_world raylib Threads::Threads)
endif()

# ---- Diagnostic probe ------------------------------------------------------
add_executable(mmo_probe src/probe_main.cpp)
target_link_libraries(mmo_probe PRIVATE mmo_world Threads::Threads)
