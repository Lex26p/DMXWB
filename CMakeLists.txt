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
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)

add_library(dmxwb_core STATIC
    src/app_info.cpp
    src/artnet_core.cpp
    src/artnet_runtime.cpp
    src/artnet_transport_linux.cpp
    src/dmx_output.cpp
    src/dmx_source_router.cpp
    src/fixture.cpp
    src/group_scene.cpp
    src/dmx_snapshot.cpp
    src/dmx_test_pattern.cpp
    src/monotonic_clock.cpp
    src/mqtt_contract.cpp
    src/mqtt_config.cpp
    src/mqtt_controller.cpp
    src/persistence.cpp
    src/persistence_storage.cpp
    src/persistence_runtime.cpp
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

add_library(dmxwb_mqtt_transport STATIC
    src/mqtt_client.cpp
    src/mqtt_runtime.cpp
)

target_include_directories(dmxwb_mqtt_transport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(dmxwb_mqtt_transport
    PUBLIC
        dmxwb_core
        PkgConfig::MOSQUITTO
)
target_compile_features(dmxwb_mqtt_transport PUBLIC cxx_std_20)
dmxwb_enable_warnings(dmxwb_mqtt_transport)

add_executable(dmxwb
    src/main.cpp
)

target_link_libraries(dmxwb PRIVATE dmxwb_core dmxwb_mqtt_transport)
target_compile_features(dmxwb PRIVATE cxx_std_20)
dmxwb_enable_warnings(dmxwb)
dmxwb_enable_portable_gnu_runtime(dmxwb)

add_executable(dmxwb_mqtt_acceptance
    src/mqtt_acceptance_main.cpp
)

target_link_libraries(dmxwb_mqtt_acceptance PRIVATE dmxwb_mqtt_transport)
target_compile_features(dmxwb_mqtt_acceptance PRIVATE cxx_std_20)
dmxwb_enable_warnings(dmxwb_mqtt_acceptance)
dmxwb_enable_portable_gnu_runtime(dmxwb_mqtt_acceptance)
set_target_properties(dmxwb_mqtt_acceptance PROPERTIES OUTPUT_NAME "dmxwb-mqtt-acceptance")

add_executable(dmxwb_artnet_acceptance
    src/artnet_acceptance_main.cpp
)

target_link_libraries(dmxwb_artnet_acceptance PRIVATE dmxwb_core)
target_compile_features(dmxwb_artnet_acceptance PRIVATE cxx_std_20)
dmxwb_enable_warnings(dmxwb_artnet_acceptance)
dmxwb_enable_portable_gnu_runtime(dmxwb_artnet_acceptance)
set_target_properties(dmxwb_artnet_acceptance PROPERTIES OUTPUT_NAME "dmxwb-artnet-acceptance")

include(CTest)
if(BUILD_TESTING)
    add_executable(dmxwb_tests
        tests/test_main.cpp
    )

    target_link_libraries(dmxwb_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_tests)

    add_executable(dmxwb_persistence_tests
        tests/test_persistence.cpp
    )

    target_link_libraries(dmxwb_persistence_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_persistence_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_persistence_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_persistence_tests)

    add_executable(dmxwb_persistence_storage_tests
        tests/test_persistence_storage.cpp
    )

    target_link_libraries(dmxwb_persistence_storage_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_persistence_storage_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_persistence_storage_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_persistence_storage_tests)

    add_executable(dmxwb_persistence_runtime_tests
        tests/test_persistence_runtime.cpp
    )

    target_link_libraries(dmxwb_persistence_runtime_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_persistence_runtime_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_persistence_runtime_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_persistence_runtime_tests)

    add_executable(dmxwb_group_scene_tests
        tests/test_group_scene.cpp
    )

    target_link_libraries(dmxwb_group_scene_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_group_scene_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_group_scene_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_group_scene_tests)

    add_executable(dmxwb_dmx_source_router_tests
        tests/test_dmx_source_router.cpp
    )

    target_link_libraries(dmxwb_dmx_source_router_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_dmx_source_router_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_dmx_source_router_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_dmx_source_router_tests)

    add_executable(dmxwb_artnet_core_tests
        tests/test_artnet_core.cpp
    )

    target_link_libraries(dmxwb_artnet_core_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_artnet_core_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_artnet_core_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_artnet_core_tests)

    add_executable(dmxwb_artnet_runtime_tests
        tests/test_artnet_runtime.cpp
    )

    target_link_libraries(dmxwb_artnet_runtime_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_artnet_runtime_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_artnet_runtime_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_artnet_runtime_tests)

    add_executable(dmxwb_artnet_transport_linux_tests
        tests/test_artnet_transport_linux.cpp
    )

    target_link_libraries(dmxwb_artnet_transport_linux_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_artnet_transport_linux_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_artnet_transport_linux_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_artnet_transport_linux_tests)

    add_executable(dmxwb_mqtt_contract_tests
        tests/test_mqtt_contract.cpp
    )

    target_link_libraries(dmxwb_mqtt_contract_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_mqtt_contract_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_mqtt_contract_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_mqtt_contract_tests)

    add_executable(dmxwb_mqtt_config_tests
        tests/test_mqtt_config.cpp
    )

    target_link_libraries(dmxwb_mqtt_config_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_mqtt_config_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_mqtt_config_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_mqtt_config_tests)

    add_executable(dmxwb_mqtt_controller_tests
        tests/test_mqtt_controller.cpp
    )

    target_link_libraries(dmxwb_mqtt_controller_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_mqtt_controller_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_mqtt_controller_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_mqtt_controller_tests)

    add_executable(dmxwb_mqtt_group_scene_tests
        tests/test_mqtt_group_scene.cpp
    )

    target_link_libraries(dmxwb_mqtt_group_scene_tests PRIVATE dmxwb_core)
    target_compile_features(dmxwb_mqtt_group_scene_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_mqtt_group_scene_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_mqtt_group_scene_tests)

    add_executable(dmxwb_mqtt_client_tests
        tests/test_mqtt_client.cpp
    )

    target_link_libraries(dmxwb_mqtt_client_tests PRIVATE dmxwb_mqtt_transport)
    target_compile_features(dmxwb_mqtt_client_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_mqtt_client_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_mqtt_client_tests)

    add_executable(dmxwb_mqtt_runtime_tests
        tests/test_mqtt_runtime.cpp
    )

    target_link_libraries(dmxwb_mqtt_runtime_tests PRIVATE dmxwb_mqtt_transport)
    target_compile_features(dmxwb_mqtt_runtime_tests PRIVATE cxx_std_20)
    dmxwb_enable_warnings(dmxwb_mqtt_runtime_tests)
    dmxwb_enable_portable_gnu_runtime(dmxwb_mqtt_runtime_tests)

    add_test(NAME dmxwb.unit COMMAND dmxwb_tests)
    add_test(NAME dmxwb.persistence COMMAND dmxwb_persistence_tests)
    add_test(NAME dmxwb.persistence_storage COMMAND dmxwb_persistence_storage_tests)
    add_test(NAME dmxwb.persistence_runtime COMMAND dmxwb_persistence_runtime_tests)
    add_test(NAME dmxwb.group_scene COMMAND dmxwb_group_scene_tests)
    add_test(NAME dmxwb.dmx_source_router COMMAND dmxwb_dmx_source_router_tests)
    add_test(NAME dmxwb.artnet_core COMMAND dmxwb_artnet_core_tests)
    add_test(NAME dmxwb.artnet_runtime COMMAND dmxwb_artnet_runtime_tests)
    add_test(NAME dmxwb.artnet_transport_linux COMMAND dmxwb_artnet_transport_linux_tests)
    add_test(NAME dmxwb.mqtt_contract COMMAND dmxwb_mqtt_contract_tests)
    add_test(NAME dmxwb.mqtt_config COMMAND dmxwb_mqtt_config_tests)
    add_test(NAME dmxwb.mqtt_controller COMMAND dmxwb_mqtt_controller_tests)
    add_test(NAME dmxwb.mqtt_group_scene COMMAND dmxwb_mqtt_group_scene_tests)
    add_test(NAME dmxwb.mqtt_client COMMAND dmxwb_mqtt_client_tests)
    add_test(NAME dmxwb.mqtt_runtime COMMAND dmxwb_mqtt_runtime_tests)
endif()
