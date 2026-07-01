cmake_minimum_required(VERSION 3.16)

project(OL VERSION 0.1.0 DESCRIPTION "small chunked 3d multiplayer engine prototype" LANGUAGES CXX C)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include(cmake/EmbedResources.cmake)

set(CMAKE_CXX_STANDARD 20 CACHE STRING "The C++ standard to use")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (WIN32)
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>" "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

set(RAYLIB_VERSION 6.0)
set(OL_RAYLIB_SOURCE_DIR "" CACHE PATH "Optional local raylib source directory")
option(OL_USE_STEAMWORKS "Build optional Steamworks networking path" OFF)
set(STEAMWORKS_SDK_DIR "" CACHE PATH "Path to Steamworks SDK root")

find_package(raylib ${RAYLIB_VERSION} QUIET)
if (NOT raylib_FOUND)
  if (OL_RAYLIB_SOURCE_DIR AND EXISTS "${OL_RAYLIB_SOURCE_DIR}/CMakeLists.txt")
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory("${OL_RAYLIB_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/raylib")
  elseif (EXISTS "${CMAKE_SOURCE_DIR}/ref/plato/out/build/x64-Debug/_deps/raylib-src/CMakeLists.txt")
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/ref/plato/out/build/x64-Debug/_deps/raylib-src" "${CMAKE_BINARY_DIR}/raylib")
  else()
    include(FetchContent)
    set(CMAKE_TLS_VERIFY FALSE)
    FetchContent_Declare(
      raylib
      DOWNLOAD_EXTRACT_TIMESTAMP OFF
      URL https://github.com/raysan5/raylib/archive/refs/tags/${RAYLIB_VERSION}.tar.gz
    )
    FetchContent_GetProperties(raylib)
    if (NOT raylib_POPULATED)
      set(FETCHCONTENT_QUIET NO)
      set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
      FetchContent_MakeAvailable(raylib)
    endif()
  endif()
endif()

if(NOT TARGET embedfile)
  add_executable(embedfile src/util/embedfile.c)
endif()
embed_resources("${OL_SOURCE_DIR}/res" OL_EMBEDDED_SOURCES OL_RES EXCLUDE_EXTENSIONS ".rc" ".ico" ".txt")

set(OL_ENGINE_SOURCES
  src/engine/world.cpp
  src/engine/physics.cpp
  src/engine/player_controller.cpp
  src/engine/render.cpp
  src/engine/net.cpp
)

set(OL_DEMO_SOURCES
  src/demo/menu.cpp
  src/demo/demo.cpp
  src/demo/main.cpp
)

add_library(ol_engine STATIC ${OL_ENGINE_SOURCES} ${OL_EMBEDDED_SOURCES})
target_include_directories(ol_engine PUBLIC src PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ol_engine PUBLIC raylib)
target_compile_definitions(ol_engine PUBLIC RAYMATH_DISABLE_CPP_OPERATORS)

add_executable(ol ${OL_DEMO_SOURCES})
target_link_libraries(ol PRIVATE ol_engine)

add_executable(ol_tests tests/engine_tests.cpp)
target_sources(ol_tests PRIVATE src/demo/menu.cpp src/demo/demo.cpp)
target_link_libraries(ol_tests PRIVATE ol_engine)

enable_testing()
add_test(NAME ol_tests_physics COMMAND ol_tests --physics)
add_test(NAME ol_tests_visual COMMAND ol_tests --visual)
add_test(NAME ol_tests_menu COMMAND ol_tests --menu)
set_tests_properties(ol_tests_physics ol_tests_visual ol_tests_menu PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

if (OL_USE_STEAMWORKS)
  if (NOT STEAMWORKS_SDK_DIR)
    message(FATAL_ERROR "OL_USE_STEAMWORKS requires STEAMWORKS_SDK_DIR")
  endif()
  if (EXISTS "${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h")
    set(OL_STEAMWORKS_ROOT "${STEAMWORKS_SDK_DIR}")
  elseif (EXISTS "${STEAMWORKS_SDK_DIR}/sdk/public/steam/steam_api.h")
    set(OL_STEAMWORKS_ROOT "${STEAMWORKS_SDK_DIR}/sdk")
  else()
    message(FATAL_ERROR "STEAMWORKS_SDK_DIR must point to a Steamworks SDK root or a directory containing sdk/public")
  endif()

  target_compile_definitions(ol_engine PUBLIC OL_USE_STEAMWORKS)
  target_include_directories(ol_engine PRIVATE "${OL_STEAMWORKS_ROOT}/public")

  if (WIN32)
    set(STEAMWORKS_LIB "${OL_STEAMWORKS_ROOT}/redistributable_bin/win64/steam_api64.lib")
    set(STEAMWORKS_BIN "${OL_STEAMWORKS_ROOT}/redistributable_bin/win64/steam_api64.dll")
  elseif (APPLE)
    set(STEAMWORKS_LIB "${OL_STEAMWORKS_ROOT}/redistributable_bin/osx/libsteam_api.dylib")
    set(STEAMWORKS_BIN "${STEAMWORKS_LIB}")
  else()
    set(STEAMWORKS_LIB "${OL_STEAMWORKS_ROOT}/redistributable_bin/linux64/libsteam_api.so")
    set(STEAMWORKS_BIN "${STEAMWORKS_LIB}")
  endif()

  target_link_libraries(ol_engine PRIVATE "${STEAMWORKS_LIB}")
  add_custom_command(TARGET ol POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${STEAMWORKS_BIN}" "$<TARGET_FILE_DIR:ol>"
    COMMAND ${CMAKE_COMMAND} -E echo "480" > "$<TARGET_FILE_DIR:ol>/steam_appid.txt"
    COMMENT "Copy Steamworks runtime and write steam_appid.txt"
  )
endif()
