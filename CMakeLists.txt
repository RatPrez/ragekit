cmake_minimum_required(VERSION 3.20)
project(ragekit VERSION 0.2.0 LANGUAGES C CXX)

# ragekit: a C++20 library for reading and writing RAGE (GTA V) resource files
# (RSC7 containers: .ydr .ydd .ytd .yft .ybn ...) and converting them to and from
# plain interchange formats. No dependency on any particular tool, and no external
# libraries: deflate comes from the vendored miniz (public domain, third_party/miniz), polygon clipping and
# offsets for the navmesh builder from the vendored Clipper2 (Boost licence, third_party/clipper2).

# The tool, tests and examples build when ragekit is the project itself; inside another project (add_subdirectory,
# FetchContent) only the library does, unless these are switched on.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_ragekit_top ON)
else()
    set(_ragekit_top OFF)
endif()
option(RAGEKIT_BUILD_CLI      "Build the rk command line tool"   ${_ragekit_top})
option(RAGEKIT_BUILD_TESTS    "Build the ragekit test programs"  ${_ragekit_top})
option(RAGEKIT_BUILD_EXAMPLES "Build the programs in examples/"  ${_ragekit_top})
option(RAGEKIT_BUILD_RENDER   "Build ragekit::render (drawables as materials, meshes and textures for a GPU)" ${_ragekit_top})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ragekit STATIC
    src/game.cpp
    src/hash.cpp
    src/rsc.cpp
    src/texture.cpp
    src/image.cpp
    src/drawable.cpp
    src/shader_gen9.cpp
    src/bounds.cpp
    src/cloth.cpp
    src/fragment.cpp
    src/particle.cpp
    src/navmesh.cpp
    src/navbuild.cpp
    src/meta.cpp
    src/map.cpp
    src/occluder.cpp
    src/pso.cpp
    src/rbf.cpp
    src/pso_view.cpp
    src/manifest.cpp
    src/rel.cpp
    src/audio_occlusion.cpp
    src/gltf.cpp
    src/xml.cpp
    src/rpf.cpp
    src/rpf_crypto.cpp
    src/rpf_write.cpp
    src/gameindex.cpp
    src/world.cpp
    src/xml_dom.cpp
    src/fxc.cpp
    src/shader_db.cpp
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_tinfl.c
    third_party/clipper2/src/clipper.engine.cpp
    third_party/clipper2/src/clipper.offset.cpp
    third_party/clipper2/src/clipper.rectclip.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(ragekit PUBLIC Threads::Threads)
add_library(ragekit::ragekit ALIAS ragekit)
# version.h must agree with the project version (the release tag is v${PROJECT_VERSION})
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/include/ragekit/version.h _ragekit_version_h)
string(REGEX MATCH "RAGEKIT_VERSION_STRING \"([0-9.]+)\"" _ragekit_v "${_ragekit_version_h}")
if (NOT CMAKE_MATCH_1 STREQUAL ragekit_VERSION)
    message(FATAL_ERROR "include/ragekit/version.h says ${CMAKE_MATCH_1}, CMakeLists.txt says ${ragekit_VERSION}")
endif()
target_compile_features(ragekit PUBLIC cxx_std_20)

target_include_directories(ragekit
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clipper2/include)
target_compile_definitions(ragekit PRIVATE MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_ZLIB_COMPAT_NAMES)

if (MSVC)
    target_compile_options(ragekit PRIVATE /W4)
else()
    target_compile_options(ragekit PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
endif()

if (RAGEKIT_BUILD_CLI)
    add_executable(rk tools/rk.cpp tools/rk_gameindex.cpp)
    target_link_libraries(rk PRIVATE ragekit)
endif()

if (RAGEKIT_BUILD_TESTS)
    enable_testing()
    add_executable(rk_rsc_test tests/rsc_test.cpp)   # RSC7 page flags and the page layout Builder makes
    target_link_libraries(rk_rsc_test PRIVATE ragekit)
    add_executable(rk_roundtrip tests/roundtrip.cpp)
    target_link_libraries(rk_roundtrip PRIVATE ragekit)
    target_include_directories(rk_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)   # cloth_io.h: fragment cloth into a test resource
    add_executable(rk_rpf_test tests/rpf_test.cpp)
    target_link_libraries(rk_rpf_test PRIVATE ragekit)
    target_include_directories(rk_rpf_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_executable(rk_ymf_full tests/ymf_full.cpp)
    target_link_libraries(rk_ymf_full PRIVATE ragekit)
    add_executable(rk_audio_test tests/audio_test.cpp)
    target_link_libraries(rk_audio_test PRIVATE ragekit)
    add_executable(rk_build_mlo tests/build_mlo.cpp)
    target_link_libraries(rk_build_mlo PRIVATE ragekit)
    add_executable(rk_xml_test tests/xml_test.cpp)   # xml: CodeWalker XML writers vs CodeWalker (tests/cmp_xml.sh)
    target_link_libraries(rk_xml_test PRIVATE ragekit)
    add_executable(rk_shaderdb_test tests/shaderdb_test.cpp)   # shader database: rules, cache, the game's layouts (with a cache argument)
    target_link_libraries(rk_shaderdb_test PRIVATE ragekit)
    add_executable(rk_texenc_test tests/texenc_test.cpp)   # block compression quality (RMS limits on synthetic images)
    target_link_libraries(rk_texenc_test PRIVATE ragekit)
    add_executable(rk_occluder_test tests/occluder_test.cpp)   # box occluders, occlude models, entities removed from a ymap
    target_link_libraries(rk_occluder_test PRIVATE ragekit)
    add_executable(rk_gen9_test tests/gen9_test.cpp)   # Legacy <-> Enhanced conversions against the game's own files (both installs)
    target_link_libraries(rk_gen9_test PRIVATE ragekit)
    target_include_directories(rk_gen9_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_executable(rk_gen_shader_table tools/gen_shader_table.cpp)   # writes src/shader_gen9.inc from a Legacy and an Enhanced install
    target_link_libraries(rk_gen_shader_table PRIVATE ragekit)
    add_executable(rk_world_test tests/world_test.cpp)   # world index against the game install: counts, MLO instance rotation, queries
    target_link_libraries(rk_world_test PRIVATE ragekit)
    add_executable(rk_robust_test tests/robust_test.cpp)   # malformed files throw: no reads past a buffer, no made-up allocations
    target_link_libraries(rk_robust_test PRIVATE ragekit)
    add_executable(rk_bench tests/bench.cpp)   # timings of the hot paths over the game install (not a test)
    target_link_libraries(rk_bench PRIVATE ragekit)
endif()

if (RAGEKIT_BUILD_EXAMPLES)
    foreach(example ydr_info ytd_export to_gltf box_collision make_interior rpf_list game_lookup)
        add_executable(${example} examples/${example}.cpp)
        target_link_libraries(${example} PRIVATE ragekit)
    endforeach()
endif()

# ragekit::render: what a renderer needs to draw the game's drawables with the game's shading (materials, meshes,
# textures), with no graphics API in it. ragekit-viewer has an OpenGL backend for it.
if (RAGEKIT_BUILD_RENDER)
    add_library(ragekit_render STATIC
        src/render/material.cpp
        src/render/model.cpp
        src/render/environment.cpp
        src/render/timecycle.cpp
        src/render/light.cpp
        src/render/particles.cpp)
    add_library(ragekit::render ALIAS ragekit_render)
    target_link_libraries(ragekit_render PUBLIC ragekit)
    if (MSVC)
        target_compile_options(ragekit_render PRIVATE /W4)
    else()
        target_compile_options(ragekit_render PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS ragekit EXPORT ragekitTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if (RAGEKIT_BUILD_RENDER)
    install(TARGETS ragekit_render EXPORT ragekitTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
