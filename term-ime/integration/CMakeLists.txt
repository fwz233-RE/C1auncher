cmake_minimum_required(VERSION 3.14)

# Normally: add_subdirectory(integration) at the END of term-ime/CMakeLists.txt.
# Standalone host verification can reuse the already-built host static archives;
# this does not pretend those x86_64 libraries are cross-compiled MIPS artifacts.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(c1-ime-integration LANGUAGES C CXX)
    enable_testing()
    set(C1_IME_EXISTING_BUILD "" CACHE PATH "Existing HOST term-ime build directory")
    if(NOT C1_IME_EXISTING_BUILD)
        message(FATAL_ERROR "Standalone host build requires -DC1_IME_EXISTING_BUILD=/absolute/term-ime/build")
    endif()
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Existing-build mode is host-only; cross-build all dependencies in the parent project")
    endif()
    get_filename_component(_term_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    add_library(term-ime-lib STATIC IMPORTED)
    set_target_properties(term-ime-lib PROPERTIES
        IMPORTED_LOCATION "${C1_IME_EXISTING_BUILD}/libtermime.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_term_root}/src;${_term_root}/deps/librime/src;${_term_root}/deps/librime/include"
        INTERFACE_LINK_LIBRARIES "${C1_IME_EXISTING_BUILD}/deps/librime/lib/librime.a;${C1_IME_EXISTING_BUILD}/_deps_stage/lib/libyaml-cpp.a;${C1_IME_EXISTING_BUILD}/_deps_stage/lib/libleveldb.a;${C1_IME_EXISTING_BUILD}/_deps_stage/lib/libmarisa.a;${C1_IME_EXISTING_BUILD}/_deps_stage/lib/libopencc.a;${C1_IME_EXISTING_BUILD}/deps/spdlog/libspdlog.a;${C1_IME_EXISTING_BUILD}/libutf8proc.a;pthread;dl;rt;m")
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "c1-ime-service and SDK currently require Linux SOCK_SEQPACKET/SO_PEERCRED")
endif()

add_library(c1-ime-client STATIC c1_ime_client.c)
target_include_directories(c1-ime-client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(c1-ime-client PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_options(c1-ime-client PRIVATE -Wall -Wextra -Werror -pedantic)

add_executable(c1-ime-service service.cpp)
set_target_properties(c1-ime-service PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_link_libraries(c1-ime-service PRIVATE term-ime-lib)
target_compile_options(c1-ime-service PRIVATE -Wall -Wextra -Werror)
option(C1_IME_STATIC_SERVICE "Link the service with static runtime libraries" ON)
if(C1_IME_STATIC_SERVICE AND NOT ENABLE_ASAN AND NOT ENABLE_UBSAN)
    target_link_options(c1-ime-service PRIVATE -static -Wl,--gc-sections)
endif()

option(C1_IME_BUILD_EXAMPLES "Build the pure C17 client example" ON)
if(C1_IME_BUILD_EXAMPLES)
    add_executable(c1-ime-client-demo examples/client_demo.c)
    target_link_libraries(c1-ime-client-demo PRIVATE c1-ime-client)
    set_target_properties(c1-ime-client-demo PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS NO LINKER_LANGUAGE C)
    target_compile_options(c1-ime-client-demo PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

option(C1_IME_BUILD_TESTS "Build C SDK and real Rime integration tests" ON)
if(C1_IME_BUILD_TESTS)
    add_executable(c1-ime-sdk-test tests/test_sdk.c)
    set_target_properties(c1-ime-sdk-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS NO LINKER_LANGUAGE C)
    target_link_libraries(c1-ime-sdk-test PRIVATE c1-ime-client)
    target_compile_options(c1-ime-sdk-test PRIVATE -Wall -Wextra -Werror -pedantic)
    add_test(NAME c1-ime-sdk COMMAND c1-ime-sdk-test)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        # Pure host-Python safety checks are valid in both host and cross trees.
        add_test(NAME c1-ime-runtime-preparation COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prepare_runtime.py")
    endif()
    if(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
        set(_c1_demo_args)
        if(C1_IME_BUILD_EXAMPLES)
            list(APPEND _c1_demo_args --demo "$<TARGET_FILE:c1-ime-client-demo>")
        endif()
        add_test(NAME c1-ime-real-rime COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_service.py"
            --service "$<TARGET_FILE:c1-ime-service>"
            --sdk-test "$<TARGET_FILE:c1-ime-sdk-test>"
            --shared-data "${CMAKE_CURRENT_SOURCE_DIR}/../data/rime-data" ${_c1_demo_args})
        set_tests_properties(c1-ime-real-rime PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
    endif()
endif()
