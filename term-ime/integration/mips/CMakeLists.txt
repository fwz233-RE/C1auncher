cmake_minimum_required(VERSION 3.22)
project(c1-ime-mips LANGUAGES C CXX)
if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "mipsel")
    message(FATAL_ERROR "Use integration/build-mips.py and its MIPS toolchain")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(_term_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT C1_MIPS_STAGE)
    message(FATAL_ERROR "C1_MIPS_STAGE must contain freshly cross-built static dependencies")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${C1_MIPS_STAGE}")

# Explicit target paths also prevent a host cache or installed package from
# silently satisfying librime's custom Find modules.
foreach(_pair "YamlCpp|yaml-cpp" "LevelDb|leveldb" "Marisa|marisa" "Opencc|opencc")
    string(REPLACE "|" ";" _parts "${_pair}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _archive)
    set(${_name}_LIBRARY "${C1_MIPS_STAGE}/lib/lib${_archive}.a" CACHE FILEPATH "Target static dependency" FORCE)
    set(${_name}_INCLUDE_PATH "${C1_MIPS_STAGE}/include" CACHE PATH "Target dependency headers" FORCE)
    if(NOT EXISTS "${${_name}_LIBRARY}")
        message(FATAL_ERROR "Missing target dependency: ${${_name}_LIBRARY}")
    endif()
endforeach()
set(X11Keysym "${C1_MIPS_STAGE}/include" CACHE PATH "Architecture-independent key constants" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_DATA OFF CACHE BOOL "" FORCE)
set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
set(ENABLE_THREADING ON CACHE BOOL "" FORCE)
set(ENABLE_TIMESTAMP OFF CACHE BOOL "" FORCE)
set(ENABLE_EXTERNAL_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory("${_term_root}/deps/librime" librime EXCLUDE_FROM_ALL)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${_term_root}/deps/spdlog" spdlog EXCLUDE_FROM_ALL)
add_library(utf8proc STATIC "${_term_root}/deps/utf8proc/utf8proc.c")
target_include_directories(utf8proc PUBLIC "${_term_root}/deps/utf8proc")
target_compile_definitions(utf8proc PUBLIC UTF8PROC_STATIC)

# Service-only IME wrapper: no terminal, FTXUI, libuv, JSON, or UI test code.
add_library(term-ime-lib STATIC
    "${_term_root}/src/ime/engine.cpp"
    "${_term_root}/src/ime/rime_engine.cpp"
    "${_term_root}/src/util/utf8.cpp")
target_include_directories(term-ime-lib PUBLIC
    "${_term_root}/src" "${_term_root}/deps/librime/src" "${_term_root}/deps/librime/include")
target_link_libraries(term-ime-lib PUBLIC rime-static spdlog::spdlog utf8proc
    "${C1_MIPS_STAGE}/lib/libmarisa.a" pthread dl rt m)
set(C1_IME_STATIC_SERVICE ON CACHE BOOL "" FORCE)
set(C1_IME_BUILD_EXAMPLES ON CACHE BOOL "" FORCE)
set(C1_IME_BUILD_TESTS ON CACHE BOOL "" FORCE)
find_program(C1_MIPS_QEMU qemu-mipsel)
if(C1_MIPS_QEMU)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${C1_MIPS_QEMU};-cpu;24Kf" CACHE STRING "Emulator for target SDK tests")
endif()
enable_testing()
add_subdirectory("${_term_root}/integration" integration)
# Service-only hardening. Static ELF has no lazy dynamic binding; -z now is
# retained explicitly, without claiming a dynamic BIND_NOW tag or full RELRO.
target_link_options(c1-ime-service PRIVATE -Wl,-z,noexecstack,-z,relro,-z,now)
# GCC 10 libstdc++/gthread weak references do not extract archive members from
# glibc >= 2.34's merged libc.a. Reproduced QEMU crashes called address zero
# from std::thread::join and LevelDB's condition_variable destructor. Resolve
# all used weak pthread entry points, not just the first observed crash.
# Merely adding -pthread/-lpthread is insufficient (libpthread.a is empty).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/pthread-symbols.txt" _c1_pthread_symbols REGEX "^[a-z_]+$")
foreach(_symbol IN LISTS _c1_pthread_symbols)
    target_link_options(c1-ime-service PRIVATE "-Wl,-u,${_symbol}")
endforeach()
