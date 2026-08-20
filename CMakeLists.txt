cmake_minimum_required(VERSION 3.18)

project(DMXWB VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "DMXWB supports Linux builds only. "
        "Use the local Linux/WSL environment for host tests and the Bullseye ARM64 cross-build for WB8.")
endif()

option(DMXWB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(DMXWB_STATIC_GNU_RUNTIME "Statically link libstdc++ and libgcc for portable GNU/Linux deployment" OFF)

function(dmxwb_enable_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
        )
        if(DMXWB_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(dmxwb_enable_portable_gnu_runtime target)
    if(DMXWB_STATIC_GNU_RUNTIME)
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            message(FATAL_ERROR "DMXWB_STATIC_GNU_RUNTIME requires GNU C++")
        endif()
        target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
    endif()
endfunction()

find_package(Threads REQUIRED)

add_library(dmxwb_core STATIC
    src/app_info.cpp
    src/dmx_output.cpp
    src/dmx_snapshot.cpp
    src/dmx_test_pattern.cpp
    src/monotonic_clock.cpp
    src/dmx_transport_linux.cpp
)

target_compile_definitions(dmxwb_core PUBLIC DMXWB_HAS_LINUX_SERIAL=1)

target_include_directories(dmxwb_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(dmxwb_core PUBLIC Threads::Threads)
target_compile_features(dmxwb_core PUBLIC cxx_std_20)
target_compile_definitions(dmxwb_core PRIVATE DMXWB_VERSION="${PROJECT_VERSION}")
dmxwb_enable_warnings(dmxwb_core)

add_executable(dmxwb
    src/main.cpp
)

target_link_libraries(dmxwb PRIVATE dmxwb_core)
target_compile_features(dmxwb PRIVATE cxx_std_20)
dmxwb_enable_warnings(dmxwb)
dmxwb_enable_portable_gnu_runtime(dmxwb)

include(CTest)
if(BUILD_TESTING)
    add_executable(dmxwb_tests
        tests/test_main.cpp
    )

    target_link_libraries(dmxwb_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_tests)

    add_test(NAME dmxwb.unit COMMAND dmxwb_tests)
endif()
