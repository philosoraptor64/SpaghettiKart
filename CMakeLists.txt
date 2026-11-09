cmake_minimum_required(VERSION 3.16.0 FATAL_ERROR)

# Set the project version and language
project(
  Spaghettify
  VERSION 1.0.0
  LANGUAGES C CXX ASM)
include(FetchContent)

# Set Project Version
include(cmake/GetVersion.cmake)

# Set some Cmake variable
include(cmake/SetCmakeVar.cmake)

# add_compile_options(-fsanitize=address) add_link_options(-fsanitize=address)

set(YAML_CPP_STATIC_DEFINE ON)
include(libultraship/cmake/cvars.cmake)

if(WIN32)
  option(ENABLE_VCPKG "Enable vcpkg for dependencies" ON)
else()
  option(ENABLE_VCPKG "Enable vcpkg for dependencies" OFF)
endif()

if(ENABLE_VCPKG)
  include(cmake/dependencies/vcpkg.cmake)
endif()

set(SPAGHETTIKART_IOS OFF)
if(IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(SPAGHETTIKART_IOS ON)
  set(PLATFORM "OS64")
  include(cmake/ios.toolchain.cmake)
  # Sign with your own Apple team: -DSPAGHETTIKART_IOS_TEAM=XXXXXXXXXX (or pass
  # DEVELOPMENT_TEAM=... to xcodebuild).
  set(SPAGHETTIKART_IOS_TEAM "" CACHE STRING "Apple development team ID for iOS signing")
  set(SPAGHETTIKART_IOS_BUNDLE_ID "io.github.philosoraptor.spaghettikart" CACHE STRING "iOS bundle identifier")
  if(SPAGHETTIKART_IOS_TEAM)
    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${SPAGHETTIKART_IOS_TEAM}")
  endif()
  set(PROJECT_ID "${SPAGHETTIKART_IOS_BUNDLE_ID}")
endif()

# Toggle for GLES (Required for many ARM/Mobile platforms)
option(USE_OPENGLES "Use OpenGLES instead of Desktop OpenGL" OFF)

# Set game compilation version
set(VERSION us)
set(SKIP_XCODE_VERSION_CHECK ON)

# TODO: Sorry i broke this
set(GFX_DEBUG_DISASSEMBLER OFF)

if(MSVC)
  set(CPP "${CMAKE_C_COMPILER}" "/EP")
else()
  set(CPP "${CMAKE_C_COMPILER}" "-E" "-P" "-Wno-trigraphs" "-x" "c")
endif()

if(WIN32)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/properties.h.in
                 ${CMAKE_CURRENT_SOURCE_DIR}/properties.h @ONLY)
endif()

# Collect source files to build the executable
file(
  GLOB ALL_FILES_ROOT
  CONFIGURE_DEPENDS
  RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
  "src/*.c"
  "src/*.h"
  "src/os/guLookAtF.c"
  "src/os/guPerspectiveF.c"
  "src/os/guMtxF2L.c"
  "src/os/guRotateF.c"
  "src/os/guOrthoF.c"
  "src/os/guScaleF.c"
  "src/os/guTranslateF.c"
  "src/os/guMtxCatF.c"
  "src/os/guMtxCatL.c"
  "src/os/guNormalize.c")

file(
  GLOB_RECURSE ALL_FILES
  CONFIGURE_DEPENDS
  RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
  "src/buffers/*.c"
  "src/buffers/*.h"
  "src/data/*.c"
  "src/data/*.h"
  "src/debug/*.c"
  "src/debug/*.h"
  "src/ending/*.c"
  "src/ending/*.h"
  "src/racing/*.c"
  "src/racing/*.h"
  "src/audio/*.c"
  "src/audio/*.h"
  "src/port/*.c"
  "src/port/*.cpp"
  "src/port/*.h"
  "src/mods/*.c"
  "src/mods/*.cpp"
  "src/mods/*.h"
  "assets/code/*.c"
  "courses/*.c"
  "courses/*.h"
  "src/engine/*.cpp"
  "src/engine/*.c"
  "src/engine/*.h"
  "src/enhancements/*.c"
  "src/enhancements/*.cpp"
  "src/enhancements/*.h"
  "Resource.rc")

list(APPEND ALL_FILES ${ALL_FILES_ROOT})

# Exclude specific files from the ALL_FILES list
list(FILTER ALL_FILES EXCLUDE REGEX ".*.inc.c")
list(FILTER ALL_FILES EXCLUDE REGEX "./src/debug/crash_screen_enhancement.c")

if(SPAGHETTIKART_IOS)
  # The old libultraship/ios folder (storyboard, plist) no longer exists; the
  # app uses ios/Info.plist.in (UIScene manifest, UILaunchScreen) instead.
  # Game data (mk64.o2r, spaghetti.o2r) lives in the app's Documents folder,
  # which is where libultraship looks on iOS; nothing ROM-derived is bundled.
  set(IOS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ios)
  set(ICON_FILES ${CMAKE_CURRENT_SOURCE_DIR}/icon.png)
  list(APPEND ALL_FILES ${ICON_FILES})
endif()

# i64 game module (Mac): the same game as a shared library that exports only
# the n64_game_* functions (src/port/i64/exports.txt; src/port/i64_module.cpp)
# for the i64 launcher, which owns the app, SDL, the window, audio and the
# network session. The stand-alone app is unchanged when this is OFF.
option(SPAGHETTIKART_I64_MODULE "Build Mario Kart as an i64 game module" OFF)
if(SPAGHETTIKART_I64_MODULE)
  add_library(${PROJECT_NAME} SHARED ${ALL_FILES})
  target_compile_definitions(${PROJECT_NAME} PRIVATE SPAGHETTIKART_I64_MODULE=1)
  set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME mk64_i64)
  target_link_options(${PROJECT_NAME} PRIVATE
    "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/port/i64/exports.txt")
else()
  add_executable(${PROJECT_NAME} ${ALL_FILES})
endif()
target_include_directories(
  ${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                          ${CMAKE_CURRENT_SOURCE_DIR}/include
                          ${CMAKE_CURRENT_SOURCE_DIR}/include/assets
                          ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(${PROJECT_NAME} PRIVATE YAML_CPP_STATIC_DEFINE)

# Rollback snapshots: put the game's mutable C globals in the __MKSNAP segment
# (src/port/SnapshotSection.h). -fno-common turns tentative definitions into
# real ones so the section pragma applies to them. Netplay transport, the
# replay checker and audio keep the default sections.
if(APPLE AND CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-fno-common>
    $<$<COMPILE_LANGUAGE:C>:-include>
    "$<$<COMPILE_LANGUAGE:C>:${CMAKE_CURRENT_SOURCE_DIR}/src/port/SnapshotSection.h>")
  file(GLOB MK_SNAPSHOT_EXCLUDED "src/audio/*.c" "src/buffers/audio_heap.c")
  list(APPEND MK_SNAPSHOT_EXCLUDED src/port/KartNet.c src/port/ReplayCheck.c src/profiler.c)
  set_source_files_properties(${MK_SNAPSHOT_EXCLUDED} PROPERTIES COMPILE_DEFINITIONS MK_SNAPSHOT_EXCLUDE=1)
endif()

if(SPAGHETTIKART_IOS)
  target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_IOS=1)
  set_xcode_property(${PROJECT_NAME} PRODUCT_BUNDLE_IDENTIFIER ${PROJECT_ID}
                     All)
  set_target_properties(
    ${PROJECT_NAME}
    PROPERTIES MACOSX_BUNDLE TRUE
               MACOSX_BUNDLE_INFO_PLIST ${IOS_DIR}/Info.plist.in
               MACOSX_BUNDLE_GUI_IDENTIFIER ${PROJECT_ID}
               MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
               MACOSX_BUNDLE_BUNDLE_VERSION "$(CURRENT_PROJECT_VERSION)"
               RESOURCE "${ICON_FILES}"
               XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
endif()

include(cmake/SetFlags.cmake)

# ##############################################################################
# MSVC runtime library
# ##############################################################################
if(MSVC)
  set_property(
    TARGET ${PROJECT_NAME}
    PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(cmake/dependencies/common.cmake)

include(cmake/dependencies/FindLib.cmake)

# n64net (github.com/philosoraptor64/n64net): netplay input exchange with
# prediction/rollback, time sync and sync checks (used by KartNet's rollback
# and lockstep modes) and voice chat. After the dependencies, so it finds
# libultraship's SDL2 target (iOS has no installed SDL2).
set(N64NET_VOICE ON CACHE BOOL "" FORCE)
add_subdirectory(n64net)
target_link_libraries(${PROJECT_NAME} PRIVATE n64net)
target_compile_definitions(${PROJECT_NAME} PRIVATE KARTNET_VOICE=1)
# Netplay build id: the room server pairs only equal simulation code.
n64net_add_build_id(${PROJECT_NAME}
  HEADER ${CMAKE_BINARY_DIR}/generated/kartnet_build_id.h
  MACRO KARTNET_BUILD_ID
  ENTRIES "${CMAKE_SOURCE_DIR}::src"
          "${CMAKE_SOURCE_DIR}/n64net::client/src/n64net_input.cpp"
          "${CMAKE_SOURCE_DIR}/n64net::client/include/n64net_input.h")


# fmt's consteval format-string checking fails to compile under newer
# AppleClang (Xcode 16+) with the spdlog/fmt versions in use (both vcpkg's on
# CI and Torch's bundled copy — see cmake/TorchExternalFixes.cmake for the
# latter):
#   error: call to consteval function 'fmt::basic_format_string<...>' is not
#   a constant expression
# Defining FMT_CONSTEVAL as empty falls back to fmt's pre-C++20 constexpr
# checking, which compiles cleanly with identical runtime behavior. Scoped to
# AppleClang; other platforms and compilers are untouched.
# TODO: Temporary hack. Remove this (plus cmake/TorchExternalFixes.cmake and
# the vcpkg.json fmt/spdlog pins) once the Torch pin is bumped past Torch's
# spdlog/fmt update. Note the bump also needs matching MK64 loader changes,
# see the discussion on PR #712.
if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  foreach(FMT_CONSUMER ${PROJECT_NAME} libultraship torch)
    target_compile_definitions(${FMT_CONSUMER} PRIVATE "FMT_CONSTEVAL=")
  endforeach()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")

  nx_generate_nacp(
    ${PROJECT_NAME}.nacp
    NAME
    "${PROJECT_NAME}"
    AUTHOR
    "${PROJECT_TEAM}"
    VERSION
    "${PROJECT_VERSION}")

  nx_create_nro(${PROJECT_NAME} NACP ${PROJECT_NAME}.nacp ICON
                ${CMAKE_CURRENT_SOURCE_DIR}/icon.png)

  install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.nro
    DESTINATION .
    COMPONENT ${PROJECT_NAME})
endif()

# ##############################################################################
# Compile and link options
# ##############################################################################


# On macOS these live in the .app's Contents/Resources instead (see
# cmake/macos/apple_bundle.cmake); copying them next to the executable would
# just duplicate them inside Contents/MacOS.
if(NOT CMAKE_SYSTEM_NAME MATCHES "Darwin")
add_custom_command(
  TARGET ${PROJECT_NAME}
  POST_BUILD
  COMMENT "Copying asset yamls..."
  COMMAND
    ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DDESTINATION_DIR=$<TARGET_FILE_DIR:${PROJECT_NAME}>
    -P ${CMAKE_SOURCE_DIR}/cmake/StageRuntimeFiles.cmake
  VERBATIM)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch" AND NOT SPAGHETTIKART_IOS)
  include(ExternalProject)
  # Torch's pinned spdlog bundles an fmt that trips a consteval error under
  # newer AppleClang (Xcode 16+): "call to consteval function
  # 'fmt::basic_format_string<...>' is not a constant expression". Neutralize
  # FMT_CONSTEVAL inside the Torch sub-build via CMAKE_PROJECT_INCLUDE (Torch's
  # CMakeLists overwrites CMAKE_CXX_FLAGS, so flags can't be injected that
  # way). Fixes the build-macos-arm64 job; the intel runner's older Xcode
  # doesn't hit it.
  # TODO: Temporary hack. Remove the CMAKE_PROJECT_INCLUDE line below (plus
  # cmake/TorchExternalFixes.cmake itself) once the Torch pin is bumped past
  # Torch's spdlog/fmt update, see the discussion on PR #712.
  ExternalProject_Add(TorchExternal
    PREFIX TorchExternal
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/torch
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/torch
               -DCMAKE_PROJECT_INCLUDE=${CMAKE_SOURCE_DIR}/cmake/TorchExternalFixes.cmake
               -DUSE_STANDALONE=ON
               -DBUILD_STORMLIB=${BUILD_STORMLIB}
               -DBUILD_SM64=${BUILD_SM64}
               -DBUILD_MK64=${BUILD_MK64}
               -DBUILD_SF64=${BUILD_SF64}
               -DBUILD_FZERO=${BUILD_FZERO}
               -DBUILD_MARIO_ARTIST=${BUILD_MARIO_ARTIST}
               -DBUILD_NAUDIO=${BUILD_NAUDIO}
  )

  ExternalProject_Get_Property(TorchExternal install_dir)
  if(MSVC)
    set(TORCH_EXECUTABLE
        ${install_dir}/src/TorchExternal-build/$<CONFIGURATION>/torch)
  else()
    set(TORCH_EXECUTABLE ${install_dir}/src/TorchExternal-build/torch)
  endif()

  add_custom_target(
    ExtractAssets
    DEPENDS TorchExternal
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMAND ${TORCH_EXECUTABLE} header -o baserom.us.z64
    COMMAND ${TORCH_EXECUTABLE} o2r baserom.us.z64 --additional-files
            meta/mods.toml
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/mk64.o2r"
            "${CMAKE_BINARY_DIR}/mk64.o2r"
    COMMAND
      ${CMAKE_COMMAND} -DTORCH_EXECUTABLE=${TORCH_EXECUTABLE}
      -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DBINARY_DIR=${CMAKE_BINARY_DIR}
      -P ${CMAKE_SOURCE_DIR}/cmake/GenerateO2R.cmake
    VERBATIM)

  add_custom_target(
    GenerateO2R
    DEPENDS TorchExternal
    COMMAND
      ${CMAKE_COMMAND} -DTORCH_EXECUTABLE=${TORCH_EXECUTABLE}
      -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DBINARY_DIR=${CMAKE_BINARY_DIR}
      -P ${CMAKE_SOURCE_DIR}/cmake/GenerateO2R.cmake
    VERBATIM)

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(
      FILES "${CMAKE_BINARY_DIR}/spaghetti.o2r"
      DESTINATION .
      COMPONENT ${PROJECT_NAME})
  endif()

  if(APPLE AND NOT SPAGHETTIKART_IOS AND NOT SPAGHETTIKART_I64_MODULE)
    configure_file("${CMAKE_SOURCE_DIR}/Info.plist"
                   "${CMAKE_BINARY_DIR}/Info.plist" COPYONLY)
    # Assemble SpaghettiKart.app: app icon, bundle metadata, resources, dylib
    # relinking, and ad-hoc codesigning. Also generates the icns (superseding
    # the old CreateOSXIcons target).
    include(${CMAKE_SOURCE_DIR}/cmake/macos/apple_bundle.cmake)
  endif()

  set_property(TARGET ${PROJECT_NAME} PROPERTY APPIMAGE_DESKTOP_FILE_TERMINAL
                                               YES)
  set_property(
    TARGET ${PROJECT_NAME} PROPERTY APPIMAGE_DESKTOP_FILE
                                    "${CMAKE_SOURCE_DIR}/SpaghettiKart.desktop")
  set_property(TARGET ${PROJECT_NAME} PROPERTY APPIMAGE_ICON_FILE
                                               "${CMAKE_SOURCE_DIR}/icon.png")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CPACK_GENERATOR "External")
elseif(CMAKE_SYSTEM_NAME MATCHES "Windows|NintendoSwitch|CafeOS")
  set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
  set(CPACK_GENERATOR "Bundle")
endif()

set(CPACK_PROJECT_CONFIG_FILE
    ${CMAKE_SOURCE_DIR}/cmake/configure-packaging.cmake)
include(cmake/packaging.cmake)
