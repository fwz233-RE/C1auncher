cmake_minimum_required(VERSION 3.14)
project(term-ime VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
# Release: optimize for size + dead-code elimination + link-time optimization.
# -Os + -ffunction-sections/-fdata-sections + --gc-sections drops the fully-static
# binary from ~8.1M to ~6.1M (FTXUI/librime unused modules discarded); -flto then
# removes cross-TU unreferenced template instantiations (esp. std::regex) for
# ~6.1M -> ~6.1M (stripped ~4.9M). Function/data sections + gc-sections are
# also added to the final link for the term-ime executable below.
set(CMAKE_CXX_FLAGS_RELEASE "-Os -ffunction-sections -fdata-sections -flto")
set(CMAKE_C_FLAGS_RELEASE "-Os -ffunction-sections -fdata-sections -flto")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,--gc-sections -flto")

# ============================================================
# Sanitizers (product-grade hardening / leak detection)
# ============================================================
option(ENABLE_ASAN "Enable AddressSanitizer + LeakSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
if(ENABLE_ASAN)
    message(STATUS "Enabling AddressSanitizer + LeakSanitizer")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()
if(ENABLE_UBSAN)
    message(STATUS "Enabling UndefinedBehaviorSanitizer")
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
endif()

# ============================================================
# Dependencies
# ============================================================
add_subdirectory(deps/ftxui)
add_subdirectory(deps/spdlog)
add_subdirectory(deps/json)
add_subdirectory(deps/googletest)

# Build librime from source (minimal configuration). Self-contained build
# (no system librime-dev dependency) for reproducibility across distros.

# ----------------------------------------------------------------
# librime's transitive deps — build vendored sources as STATIC archives
# ----------------------------------------------------------------
# librime's find modules (deps/librime/cmake/Find*.cmake) use find_library/
# find_path, so they resolve to installed files, not add_subdirectory targets.
# We mirror librime's own deps.mk: configure+build+install each vendored dep
# (yaml-cpp, leveldb, marisa, opencc) into a private stage prefix, then put it
# on CMAKE_PREFIX_PATH so librime's find_package picks up the static .a + headers
# instead of system .so. BUILD_STATIC=ON (set below) flips every *_STATIC flag
# so the find modules prefer .a and define the correct static guards.
#
# leveldb builds WITHOUT snappy (check_library_exists finds none in our static
# stage -> HAVE_SNAPPY stays OFF), eliminating the snappy runtime dep.
# opencc builds with its own bundled marisa (USE_SYSTEM_MARISA=OFF) but does not
# embed it (libopencc.a has no marisa:: symbols), so librime's own marisa link
# satisfies it — no duplicate.
set(_RIME_DEPS_STAGE ${CMAKE_BINARY_DIR}/_deps_stage)
set(TERM_IME_BUILD_JOBS "2" CACHE STRING "Parallel jobs for vendored dependency bootstrap")
set(_RIME_DEPS
    ${CMAKE_SOURCE_DIR}/deps/librime/deps/yaml-cpp
    ${CMAKE_SOURCE_DIR}/deps/librime/deps/leveldb
    ${CMAKE_SOURCE_DIR}/deps/librime/deps/marisa-trie
    ${CMAKE_SOURCE_DIR}/deps/librime/deps/opencc)
set(_RIME_DEPS_STAMP ${_RIME_DEPS_STAGE}/.installed)
if(NOT EXISTS ${_RIME_DEPS_STAMP})
    message(STATUS "Building vendored librime deps (yaml-cpp/leveldb/marisa/opencc) -> ${_RIME_DEPS_STAGE}")
    file(MAKE_DIRECTORY ${_RIME_DEPS_STAGE})
    set(_DEP_COMMON -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release
                    -DCMAKE_INSTALL_PREFIX=${_RIME_DEPS_STAGE}
                    -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
    # yaml-cpp
    execute_process(COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR}/deps/librime/deps/yaml-cpp
                            -B ${CMAKE_BINARY_DIR}/_deps_build/yaml-cpp
                            ${_DEP_COMMON} -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF
                            -DYAML_CPP_BUILD_CONTRIB=OFF -DYAML_CPP_INSTALL=ON RESULT_VARIABLE _rc)
    if(_rc EQUAL 0)
        execute_process(COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/_deps_build/yaml-cpp
                                --target install -j${TERM_IME_BUILD_JOBS} RESULT_VARIABLE _rc)
    endif()
    # leveldb (no snappy in this static env)
    if(_rc EQUAL 0)
        execute_process(COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR}/deps/librime/deps/leveldb
                                -B ${CMAKE_BINARY_DIR}/_deps_build/leveldb
                                ${_DEP_COMMON} -DLEVELDB_BUILD_TESTS=OFF -DLEVELDB_BUILD_BENCHMARKS=OFF
                                RESULT_VARIABLE _rc)
    endif()
    if(_rc EQUAL 0)
        execute_process(COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/_deps_build/leveldb
                                --target install -j${TERM_IME_BUILD_JOBS} RESULT_VARIABLE _rc)
    endif()
    # marisa-trie
    if(_rc EQUAL 0)
        execute_process(COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR}/deps/librime/deps/marisa-trie
                                -B ${CMAKE_BINARY_DIR}/_deps_build/marisa ${_DEP_COMMON} RESULT_VARIABLE _rc)
    endif()
    if(_rc EQUAL 0)
        execute_process(COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/_deps_build/marisa
                                --target install -j${TERM_IME_BUILD_JOBS} RESULT_VARIABLE _rc)
    endif()
    # opencc (bundled marisa, but not embedded)
    if(_rc EQUAL 0)
        execute_process(COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR}/deps/librime/deps/opencc
                                -B ${CMAKE_BINARY_DIR}/_deps_build/opencc
                                ${_DEP_COMMON} -DUSE_SYSTEM_MARISA=OFF -DBUILD_TESTING=OFF
                                -DOPENCC_BUILD_TOOLS=OFF RESULT_VARIABLE _rc)
    endif()
    if(_rc EQUAL 0)
        execute_process(COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/_deps_build/opencc
                                --target install -j${TERM_IME_BUILD_JOBS} RESULT_VARIABLE _rc)
    endif()
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "Failed to build a vendored librime dep (rc=${_rc}). See ${CMAKE_BINARY_DIR}/_deps_build/.")
    endif()
    file(WRITE ${_RIME_DEPS_STAMP} "done\n")
endif()
list(PREPEND CMAKE_PREFIX_PATH ${_RIME_DEPS_STAGE})

# Boost: only the HEADER-ONLY libs are used (signals2, unordered_map/set,
# algorithm/string, crc) — librime was patched to use std::regex instead of
# boost::regex, so no compiled boost_regex/filesystem/system lib is needed and
# there is no ICU. find_package(Boost) (no COMPONENTS) pulls just the headers;
# Boost::regex/filesystem/system are NOT linked. (System Boost headers are a
# build-time-only dep; they compile into our binary — zero runtime Boost dep.)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_DATA OFF CACHE BOOL "" FORCE)
set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
set(ENABLE_THREADING ON CACHE BOOL "" FORCE)
set(ENABLE_TIMESTAMP OFF CACHE BOOL "" FORCE)
# BUILD_STATIC makes librime's find modules prefer .a (via *_STATIC) and set the
# static-define guards (YAML_CPP_STATIC_DEFINE, Opencc_BUILT_AS_STATIC, etc.).
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory(deps/librime EXCLUDE_FROM_ALL)

# ----------------------------------------------------------------
# libuv (bundled submodule, no system package)
# ----------------------------------------------------------------
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/libuv EXCLUDE_FROM_ALL)
# libuv defines `uv_a` (static) which propagates its own include dirs.
set(LIBUV_TARGET uv_a)
message(STATUS "Using bundled libuv (target: uv_a)")

# SML (State Machine Library) - header-only
set(SML_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/deps/sml/include)

# utf8proc - UTF-8 processing library
add_library(utf8proc STATIC
    deps/utf8proc/utf8proc.c
)
target_include_directories(utf8proc PUBLIC ${CMAKE_SOURCE_DIR}/deps/utf8proc)
target_compile_definitions(utf8proc PRIVATE UTF8PROC_STATIC)

# ============================================================
# IME Library (independent, reusable)
# ============================================================
add_library(term-ime-lib STATIC
    src/ime/engine.cpp
    src/ime/rime_engine.cpp
    src/ime/language.cpp
    src/ime/kaomoji.cpp
    src/util/utf8.cpp
    src/util/logger.cpp
    src/util/i18n.cpp
)

target_include_directories(term-ime-lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
        ${CMAKE_SOURCE_DIR}/deps/librime/include
        ${CMAKE_SOURCE_DIR}/deps/librime/src
)

target_link_libraries(term-ime-lib
    PUBLIC rime-static
    PUBLIC nlohmann_json::nlohmann_json
    PUBLIC spdlog::spdlog
    PUBLIC utf8proc
)
# Boost: fully removed. librime's boost::algorithm/signals2/interprocess/crc/uuid
# are replaced by <rime/*.hpp> shims (std::regex replaced boost::regex earlier);
# the only remaining <boost/...> include is boost/sml.hpp, provided by the
# bundled deps/sml submodule (SML_INCLUDE_DIR), not by system Boost. No
# find_package(Boost), no libboost-all-dev build dep.

set_target_properties(term-ime-lib PROPERTIES
    OUTPUT_NAME "termime"
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

# ============================================================
# Terminal Library
# ============================================================
add_library(term-terminal STATIC
    src/terminal/pty.cpp
    src/terminal/screen.cpp
    src/terminal/parser.cpp
    src/ui/renderer.cpp
    src/ui/jsx.cpp
    src/ui/components.cpp
    src/ui/settings.cpp
)

target_include_directories(term-terminal
    PUBLIC ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(term-terminal
    PUBLIC term-ime-lib
    PRIVATE util
    PRIVATE spdlog::spdlog
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE ftxui::dom
    PRIVATE ftxui::screen
)

# ============================================================
# Core Event System
# ============================================================
add_library(term-core STATIC
    src/core/event_loop.cpp
    src/core/app.cpp
    src/core/config.cpp
    src/core/input_processor.cpp
)

target_include_directories(term-core
    PUBLIC ${CMAKE_SOURCE_DIR}/src
    PUBLIC ${SML_INCLUDE_DIR}
)

target_link_libraries(term-core
    PUBLIC term-terminal
    PUBLIC term-ime-lib
    PUBLIC ${LIBUV_TARGET}
    PRIVATE ftxui::dom
    PRIVATE ftxui::screen
)

# ============================================================
# Main Executable
# ============================================================
add_executable(term-ime src/main.cpp)

target_link_libraries(term-ime
    PRIVATE term-core
    PRIVATE term-terminal
    PRIVATE term-ime-lib
    PRIVATE spdlog::spdlog
    PRIVATE ftxui::dom
    PRIVATE ftxui::screen
)
# Fully static single-file executable: link glibc/libstdc++/libgcc statically too,
# so the binary has zero runtime shared-library deps (only the dynamic loader).
# forkpty (libutil) is part of static glibc, so -lutil resolves to libutil.a.
target_link_options(term-ime PRIVATE -Wl,--gc-sections)
if(NOT ENABLE_ASAN AND NOT ENABLE_UBSAN)
    target_link_options(term-ime PRIVATE -static)
endif()

# ============================================================
# Install
# ============================================================
install(TARGETS term-ime RUNTIME DESTINATION bin)

install(TARGETS term-ime-lib term-terminal term-core
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(FILES
    src/ime/engine.hpp
    src/ime/rime_engine.hpp
    src/ime/language.hpp
    DESTINATION include/term-ime
)

install(FILES
    src/util/utf8.hpp
    DESTINATION include/term-ime/util
)

install(FILES data/pinyin.dict DESTINATION share/term-ime)

# Copy data directory to build for testing
configure_file(${CMAKE_SOURCE_DIR}/data/pinyin.dict ${CMAKE_BINARY_DIR}/data/pinyin.dict COPYONLY)

# Copy translation files
file(GLOB TRANSLATION_FILES "${CMAKE_SOURCE_DIR}/data/translations/*.json")
foreach(TRANSLATION_FILE ${TRANSLATION_FILES})
    get_filename_component(FILENAME ${TRANSLATION_FILE} NAME)
    configure_file(${TRANSLATION_FILE} ${CMAKE_BINARY_DIR}/data/translations/${FILENAME} COPYONLY)
endforeach()

# ----------------------------------------------------------------
# Bundled rime-data (luna_pinyin schemas + dict) — copied to build/share/rime-data
# so the binary finds schemas with no system rime-data-luna-pinyin package.
# Includes essay.txt (the preset vocabulary frequency table) — without it,
# use_preset_vocabulary: true in luna_pinyin.dict.yaml can't load word
# frequencies, so candidates fall back to single chars sorted by Unicode (rare
# chars like 㘈 rank ahead of 你) and phrases like "你好" are missing.
# ----------------------------------------------------------------
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/share/rime-data)
file(GLOB RIME_DATA_FILES "${CMAKE_SOURCE_DIR}/data/rime-data/*.yaml")
foreach(RIME_DATA_FILE ${RIME_DATA_FILES})
    get_filename_component(NAME ${RIME_DATA_FILE} NAME)
    configure_file(${RIME_DATA_FILE} ${CMAKE_BINARY_DIR}/share/rime-data/${NAME} COPYONLY)
endforeach()
# essay.txt is the preset vocabulary table (3.7MB, ~300k phrases with weights).
configure_file(${CMAKE_SOURCE_DIR}/data/rime-data/essay.txt
               ${CMAKE_BINARY_DIR}/share/rime-data/essay.txt COPYONLY)
# Copy opencc data files for simplifier (t2s conversion) — optional, skip if not present
if(EXISTS ${CMAKE_SOURCE_DIR}/data/rime-data/opencc/t2s.json)
  file(COPY ${CMAKE_SOURCE_DIR}/data/rime-data/opencc/ DESTINATION ${CMAKE_BINARY_DIR}/share/rime-data/opencc/)
endif()
install(DIRECTORY ${CMAKE_SOURCE_DIR}/data/rime-data/
        DESTINATION share/term-ime/rime-data
        FILES_MATCHING PATTERN "*.yaml" PATTERN "essay.txt")
install(DIRECTORY ${CMAKE_SOURCE_DIR}/data/rime-data/opencc/
        DESTINATION share/term-ime/rime-data/opencc)
# Expose the build-tree bundled data dir to C++.
target_compile_definitions(term-ime-lib PUBLIC
    RIME_BUNDLED_DATA_DIR="${CMAKE_BINARY_DIR}/share/rime-data")

# ============================================================
# Tests
# ============================================================
enable_testing()

# Test executable
add_executable(term-ime-tests
    tests/test_main.cpp
    tests/test_utf8.cpp
    tests/test_config.cpp
    tests/test_config_regressions.cpp
    tests/test_ime_state.cpp
    tests/test_i18n.cpp
    tests/test_input_processor.cpp
    tests/test_app_input.cpp
    tests/test_candidate_bar.cpp
    tests/test_terminal_regressions.cpp
    tests/test_followup_regressions.cpp
)

target_link_libraries(term-ime-tests
    PRIVATE term-ime-lib
    PRIVATE term-core
    PRIVATE GTest::gtest
    PRIVATE GTest::gtest_main
    PRIVATE ftxui::dom
    PRIVATE ftxui::screen
    PRIVATE util
)

# UI JSX test
add_executable(test-ui-jsx
    tests/test_ui_jsx.cpp
)

target_link_libraries(test-ui-jsx
    PRIVATE term-terminal
    PRIVATE term-ime-lib
    PRIVATE spdlog::spdlog
    PRIVATE ftxui::dom
    PRIVATE ftxui::screen
)

# Settings test
add_executable(test-settings
    tests/test_settings.cpp
)

target_link_libraries(test-settings
    PRIVATE term-terminal
    PRIVATE term-core
    PRIVATE term-ime-lib
    PRIVATE spdlog::spdlog
    PRIVATE ftxui::dom
    PRIVATE ftxui::screen
)

# Input processor end-to-end test
add_executable(test-input-e2e
    tests/test_input_e2e.cpp
)

target_link_libraries(test-input-e2e
    PRIVATE term-core
    PRIVATE spdlog::spdlog
)

# Register tests with CTest
include(GoogleTest)
gtest_discover_tests(term-ime-tests)
add_test(NAME input-processor-smoke COMMAND test-input-e2e)
add_test(NAME ui-jsx COMMAND test-ui-jsx)
add_test(NAME settings COMMAND test-settings)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME cli-config COMMAND ${CMAKE_COMMAND} -E env
        TERM_IME_BINARY=$<TARGET_FILE:term-ime>
        ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_cli_config.py)
    add_test(NAME app-e2e COMMAND ${CMAKE_COMMAND} -E env
        TERM_IME_BINARY=$<TARGET_FILE:term-ime>
        ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_e2e.py)
    add_test(NAME app-followup-e2e COMMAND ${CMAKE_COMMAND} -E env
        TERM_IME_BINARY=$<TARGET_FILE:term-ime>
        ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_followup_e2e.py)
    set_tests_properties(cli-config PROPERTIES TIMEOUT 30)
    set_tests_properties(app-e2e app-followup-e2e PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()

