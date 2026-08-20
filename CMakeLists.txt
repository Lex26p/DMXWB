cmake_minimum_required(VERSION 3.20)

project(DMXWB VERSION 0.1.0 LANGUAGES CXX)

option(DMXWB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(dmxwb_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
        if(DMXWB_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
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

add_library(dmxwb_core STATIC
    src/app_info.cpp
    src/dmx_snapshot.cpp
    src/dmx_test_pattern.cpp
    src/monotonic_clock.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(dmxwb_core PRIVATE src/dmx_transport_linux.cpp)
    target_compile_definitions(dmxwb_core PUBLIC DMXWB_HAS_LINUX_SERIAL=1)
else()
    target_sources(dmxwb_core PRIVATE src/dmx_transport_unsupported.cpp)
    target_compile_definitions(dmxwb_core PUBLIC DMXWB_HAS_LINUX_SERIAL=0)
endif()

target_include_directories(dmxwb_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(dmxwb_core PUBLIC cxx_std_20)
target_compile_definitions(dmxwb_core PRIVATE DMXWB_VERSION="${PROJECT_VERSION}")
dmxwb_enable_warnings(dmxwb_core)

add_executable(dmxwb
    src/main.cpp
)

target_link_libraries(dmxwb PRIVATE dmxwb_core)
target_compile_features(dmxwb PRIVATE cxx_std_20)
dmxwb_enable_warnings(dmxwb)

include(CTest)
if(BUILD_TESTING)
    add_executable(dmxwb_tests
        tests/test_main.cpp
    )

    target_link_libraries(dmxwb_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_tests)

    add_test(NAME dmxwb.unit COMMAND dmxwb_tests)
endif()
