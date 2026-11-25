cmake_minimum_required(VERSION 3.22)

project(tonearm VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(TONEARM_BUILD_PLUGIN "Build the OBS plugin module" ON)
option(TONEARM_BUILD_TESTS "Build unit tests" ON)
option(TONEARM_BUILD_TOOLS "Build the headless renderer used for documentation" ${UNIX})

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# Portable core: model, layout, design tokens, animation, rasterization.
# ---------------------------------------------------------------------------
add_library(tonearm_core STATIC
  src/core/color.cpp
  src/core/image.cpp
  src/core/media.cpp
  src/core/style.cpp
  src/core/layout.cpp
  src/core/raster.cpp
  src/core/turntable.cpp
  src/core/scene.cpp
  src/core/compositor_cpu.cpp
  src/core/demo.cpp
  src/core/motion.cpp
)
target_include_directories(tonearm_core PUBLIC src)

# Platform text backend.
if(WIN32)
  target_sources(tonearm_core PRIVATE src/text/text_dwrite.cpp)
  target_link_libraries(tonearm_core PUBLIC dwrite d2d1 windowscodecs)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo fontconfig)
  target_sources(tonearm_core PRIVATE src/text/text_pango.cpp)
  target_link_libraries(tonearm_core PUBLIC PkgConfig::PANGOCAIRO)
endif()

# ---------------------------------------------------------------------------
# Media providers
# ---------------------------------------------------------------------------
add_library(tonearm_media STATIC src/media/provider.cpp src/media/motion_resolver.cpp src/media/catalog_match.cpp src/media/demo_provider.cpp src/media/scripted_provider.cpp)
target_link_libraries(tonearm_media PUBLIC tonearm_core)
if(WIN32)
  target_sources(tonearm_media PRIVATE src/media/smtc_provider.cpp src/media/wic_decode.cpp)
  target_link_libraries(tonearm_media PUBLIC windowsapp windowscodecs ole32)
endif()

# ---------------------------------------------------------------------------
# OBS plugin
# ---------------------------------------------------------------------------
if(TONEARM_BUILD_PLUGIN)
  if(WIN32)
    # The libobs SDK built from OBS's source (see docs/building.md) installs a CMake package.
    find_package(libobs REQUIRED)
  else()
    find_package(libobs QUIET)
  endif()
  if(NOT libobs_FOUND)
    # Distribution packages (e.g. Debian's libobs-dev) ship pkg-config files only.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBOBS REQUIRED IMPORTED_TARGET libobs)
    add_library(OBS::libobs ALIAS PkgConfig::LIBOBS)
  endif()

  add_library(tonearm MODULE
    src/obs/plugin-main.cpp
    src/obs/tonearm-source.cpp
    src/obs/settings.cpp
    src/obs/gpu-compositor.cpp
    src/obs/raster-worker.cpp
    src/obs/media-service.cpp
    src/obs/motion-player.cpp
  )
  target_link_libraries(tonearm PRIVATE tonearm_media OBS::libobs)
  set_target_properties(tonearm PROPERTIES PREFIX "")
  execute_process(COMMAND git rev-parse --short=8 HEAD
                  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                  OUTPUT_VARIABLE TONEARM_BUILD_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT TONEARM_BUILD_REV)
    set(TONEARM_BUILD_REV "unknown")
  endif()
  target_compile_definitions(tonearm PRIVATE TONEARM_VERSION="${PROJECT_VERSION}" TONEARM_BUILD_REV="${TONEARM_BUILD_REV}")

  if(WIN32)
    target_sources(tonearm PRIVATE src/obs/remote-motion-source.cpp src/obs/hls-cache.cpp src/obs/motion-index.cpp)
    target_link_libraries(tonearm PRIVATE winhttp)
    configure_file(cmake/tonearm.rc.in ${CMAKE_CURRENT_BINARY_DIR}/tonearm.rc @ONLY)
    target_sources(tonearm PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/tonearm.rc)
    # Standard OBS plugin layout: obs-plugins/64bit/tonearm.dll + data/obs-plugins/tonearm/
    install(TARGETS tonearm LIBRARY DESTINATION obs-plugins/64bit RUNTIME DESTINATION obs-plugins/64bit)
    install(FILES $<TARGET_PDB_FILE:tonearm> DESTINATION obs-plugins/64bit OPTIONAL)
    install(DIRECTORY data/ DESTINATION data/obs-plugins/tonearm)
  else()
    install(TARGETS tonearm LIBRARY DESTINATION lib/obs-plugins)
    install(DIRECTORY data/ DESTINATION share/obs/obs-plugins/tonearm)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tools and tests
# ---------------------------------------------------------------------------
if(TONEARM_BUILD_TOOLS AND NOT WIN32)
  add_executable(tonearm-render tools/tonearm-render/main.cpp tools/tonearm-render/png.cpp)
  target_link_libraries(tonearm-render PRIVATE tonearm_media)
  target_compile_definitions(tonearm-render PRIVATE TONEARM_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
endif()

if(TONEARM_BUILD_TESTS)
  enable_testing()
  add_executable(tonearm-tests tests/main.cpp tests/test_color.cpp tests/test_image.cpp tests/test_media.cpp
                               tests/test_layout.cpp tests/test_scene.cpp tests/test_raster.cpp
                               tests/test_text.cpp tests/test_motion.cpp tests/test_catalog_match.cpp)
  target_link_libraries(tonearm-tests PRIVATE tonearm_media)
  target_compile_definitions(tonearm-tests PRIVATE TONEARM_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  add_test(NAME tonearm-tests COMMAND tonearm-tests)
  if(WIN32)
    add_executable(tonearm-hls-cache-smoke tests/hls-cache-smoke.cpp src/obs/hls-cache.cpp src/obs/motion-index.cpp)
    target_include_directories(tonearm-hls-cache-smoke PRIVATE src)
    target_link_libraries(tonearm-hls-cache-smoke PRIVATE winhttp)
  endif()
endif()
