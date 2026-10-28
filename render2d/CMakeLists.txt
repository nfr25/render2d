# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)   # PROJECT_IS_TOP_LEVEL (notions §49)
project(render2d LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for VS Code IntelliSense

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Warnings on our own code only.
add_library(r2d_warnings INTERFACE)
target_compile_options(r2d_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)

# Dependencies (MSYS2: mingw-w64-ucrt-x86_64-cairo, found through pkg-config).
find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
# MSYS2: mingw-w64-ucrt-x86_64-gdal. Provides the GDAL::GDAL target.
find_package(GDAL REQUIRED)
# MSYS2: mingw-w64-ucrt-x86_64-yaml-cpp.
pkg_check_modules(YAMLCPP REQUIRED IMPORTED_TARGET yaml-cpp)

# --- core: pure maths, no dependency (notions §3 to §8) ---------------------
add_library(r2d_core STATIC
    src/core/affine.cpp
    src/core/view.cpp
)
target_include_directories(r2d_core PUBLIC src)
target_link_libraries(r2d_core PRIVATE r2d_warnings)

# --- canvas: the drawing interface, header-only, no Cairo (notions §10) -----
add_library(r2d_canvas INTERFACE)
target_link_libraries(r2d_canvas INTERFACE r2d_core)

# --- backend/cairo: the only place that knows Cairo (notions §13) -----------
add_library(r2d_cairo STATIC
    src/backend/cairo/cairo_canvas.cpp
    src/backend/cairo/cairo_target.cpp
    src/backend/cairo/cairo_image.cpp
)
target_link_libraries(r2d_cairo PUBLIC r2d_canvas PkgConfig::CAIRO PRIVATE r2d_warnings)

# --- backend/gdal: the only place that knows GDAL (notions §17) -------------
add_library(r2d_gdal STATIC
    src/backend/gdal/gdal_convert.cpp
    src/backend/gdal/gdal_layer.cpp
    src/backend/gdal/gdal_raster.cpp   # the rasters: the view's window, to pixels (§65, §66)
)
target_link_libraries(r2d_gdal PUBLIC r2d_core PRIVATE GDAL::GDAL r2d_warnings)

# --- style: the SLD model, expressions and the YSLD reader (§22 to §29, §35) --
add_library(r2d_style STATIC
    src/style/expression.cpp
    src/style/ysld_reader.cpp
    src/style/ysld_writer.cpp     # the reader's twin, for editor2d's style dialog (§59)
    src/style/catalog.cpp         # what each symbolizer can receive, for editor2d's "+" (§60)
    src/style/classify.cpp        # values, bounds, rules: editor2d's Categorize and Graduate (§62)
)
target_link_libraries(r2d_style PUBLIC r2d_canvas PRIVATE PkgConfig::YAMLCPP r2d_warnings)

# --- render: symbolizers and passes, canvas + style model only (§19, §25) ----
add_library(r2d_render STATIC
    src/render/rule_matcher.cpp
    src/render/hit_test.cpp
    src/render/query_layer.cpp
    src/render/symbolize.cpp
    src/render/labels.cpp
    src/render/render_layer.cpp
)
target_link_libraries(r2d_render PUBLIC r2d_canvas r2d_style PRIVATE r2d_warnings)

# --- map: layers kept open, styles read once, the report (§45 to §47) ----------
# Draws on any Canvas: no Cairo here, the program chooses the surface.
add_library(r2d_map STATIC
    src/map/map.cpp
    src/map/config.cpp      # viewer.yaml: --config, and editor2d (§57)
)
target_link_libraries(r2d_map PUBLIC r2d_gdal r2d_style r2d_render PRIVATE PkgConfig::YAMLCPP r2d_warnings)

# --- programs and tests ---------------------------------------------------------
# Built when render2d is the main project. When another project includes it
# with add_subdirectory (editor2d), it only gets the libraries above.
# notions §49. Override with -DR2D_BUILD_PROGRAMS=ON.
option(R2D_BUILD_PROGRAMS "Build render2d's programs and tests" ${PROJECT_IS_TOP_LEVEL})
if(NOT R2D_BUILD_PROGRAMS)
    return()
endif()

add_executable(render2d_demo src/app/main.cpp)          # step 1 test scene
target_link_libraries(render2d_demo PRIVATE r2d_cairo r2d_warnings)

add_executable(render2d src/app/render2d.cpp)            # the command (step 2)
target_link_libraries(render2d PRIVATE r2d_map r2d_cairo r2d_warnings)

# --- tests ------------------------------------------------------------------
enable_testing()

add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE r2d_core r2d_warnings)
add_test(NAME core COMMAND test_core)

add_executable(test_cairo tests/test_cairo.cpp)
target_link_libraries(test_cairo PRIVATE r2d_cairo r2d_warnings)
add_test(NAME cairo COMMAND test_cairo)

add_executable(test_gdal tests/test_gdal.cpp)
target_link_libraries(test_gdal PRIVATE r2d_gdal r2d_warnings)
add_test(NAME gdal COMMAND test_gdal)

add_executable(test_style tests/test_style.cpp)
target_link_libraries(test_style PRIVATE r2d_style r2d_warnings)
add_test(NAME style COMMAND test_style)

add_executable(test_filter tests/test_filter.cpp)
target_link_libraries(test_filter PRIVATE r2d_style r2d_warnings)
add_test(NAME filter COMMAND test_filter)

add_executable(test_render tests/test_render.cpp)
target_link_libraries(test_render PRIVATE r2d_render r2d_cairo r2d_warnings)
add_test(NAME render COMMAND test_render)

add_executable(test_map tests/test_map.cpp)
target_link_libraries(test_map PRIVATE r2d_map r2d_cairo r2d_warnings)
add_test(NAME map COMMAND test_map)

add_executable(test_config tests/test_config.cpp)   # moved from editor2d (§57)
target_link_libraries(test_config PRIVATE r2d_map r2d_warnings)
add_test(NAME config COMMAND test_config)

add_executable(test_ysld_writer tests/test_ysld_writer.cpp)   # §59: the round trip
target_link_libraries(test_ysld_writer PRIVATE r2d_map r2d_cairo r2d_warnings)
target_compile_definitions(test_ysld_writer PRIVATE R2D_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME ysld_writer COMMAND test_ysld_writer)

add_executable(test_catalog tests/test_catalog.cpp)   # §60: add, write, read, remove
target_link_libraries(test_catalog PRIVATE r2d_style r2d_warnings)
add_test(NAME catalog COMMAND test_catalog)

add_executable(test_classify tests/test_classify.cpp)   # §62: the numbers of editor2d §49
target_link_libraries(test_classify PRIVATE r2d_style r2d_warnings)
add_test(NAME classify COMMAND test_classify)

add_executable(test_raster tests/test_raster.cpp)   # §65, §66: the checkerboard and the DSM
target_link_libraries(test_raster PRIVATE r2d_map r2d_cairo GDAL::GDAL r2d_warnings)   # GDAL: to write the GeoTIFFs
add_test(NAME raster COMMAND test_raster)

add_executable(test_hit tests/test_hit.cpp)
target_link_libraries(test_hit PRIVATE r2d_render r2d_warnings)
add_test(NAME hit COMMAND test_hit)
