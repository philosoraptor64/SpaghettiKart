if(WIN32)
  find_package(Ogg CONFIG REQUIRED)
  find_package(Vorbis CONFIG REQUIRED)
elseif(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  set(ADDITIONAL_LIBRARY_DEPENDENCIES -lglad SDL2::SDL2)
elseif(CMAKE_SYSTEM_NAME STREQUAL "CafeOS")
  set(ADDITIONAL_LIBRARY_DEPENDENCIES "$<$<CONFIG:Debug>:-Wl,--wrap=abort>")
  target_include_directories(${PROJECT_NAME} PRIVATE
                             ${DEVKITPRO}/portlibs/wiiu/include/)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  # No iOS builds of libogg/libvorbis exist on the host, so build them from
  # source for the device. Both define the Ogg:: / Vorbis:: targets used below.
  include(FetchContent)
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_TESTING OFF)
  set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  # libogg 1.3.5 / libvorbis 1.3.7 still declare cmake_minimum_required < 3.5.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  FetchContent_Declare(
    ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG v1.3.5
    OVERRIDE_FIND_PACKAGE)
  FetchContent_MakeAvailable(ogg)
  FetchContent_Declare(
    vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG v1.3.7)
  FetchContent_MakeAvailable(vorbis)
  # libvorbis 1.3.7 does not define the namespaced aliases.
  foreach(_lib vorbis vorbisenc vorbisfile)
    if(NOT TARGET Vorbis::${_lib})
      add_library(Vorbis::${_lib} ALIAS ${_lib})
    endif()
  endforeach()
  if(NOT TARGET Ogg::ogg)
    add_library(Ogg::ogg ALIAS ogg)
  endif()
else()
  find_package(Ogg REQUIRED)
  find_package(Vorbis REQUIRED)
endif()

if(NOT CMAKE_SYSTEM_NAME MATCHES "NintendoSwitch|CafeOS")
  set(ADDITIONAL_LIBRARY_DEPENDENCIES Ogg::ogg Vorbis::vorbis
                                      Vorbis::vorbisenc Vorbis::vorbisfile)
endif()

if(UNIX AND NOT APPLE)
  if(USE_OPENGLES)
    find_library(GLESv2_LIBRARY GLESv2 REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${GLESv2_LIBRARY})
  else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenGL::GL)
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  find_package(SDL2)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})

if(NOT USE_OPENGLES)
  target_include_directories(${PROJECT_NAME} PRIVATE ${GLEW_INCLUDE_DIRS})
endif()

target_link_libraries(${PROJECT_NAME}
                      PRIVATE torch ${ADDITIONAL_LIBRARY_DEPENDENCIES})
