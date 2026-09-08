cmake_minimum_required(VERSION 3.20)
project(GekkoPAK VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(GEKKOPAK_WARNINGS /W4 /permissive-)
else()
    set(GEKKOPAK_WARNINGS -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# gekkopak_core -- the shared protocol/device state machine.
#
# Freestanding by contract: this is compiled verbatim into the Azahar core and
# into RP2040 firmware, so it must not acquire a dependency on the host runtime.
# Exceptions and RTTI are disabled here to keep that honest.
# ---------------------------------------------------------------------------
add_library(gekkopak_core STATIC
    src/device.cpp
)

target_include_directories(gekkopak_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(gekkopak_core PUBLIC cxx_std_17)
target_compile_options(gekkopak_core PRIVATE ${GEKKOPAK_WARNINGS})
if(NOT MSVC)
    target_compile_options(gekkopak_core PRIVATE -fno-exceptions -fno-rtti)
endif()

# ---------------------------------------------------------------------------
# gekkopak -- the host-side analytic performance model. Separate from the
# protocol core on purpose: it estimates cost, it does not speak the wire.
# ---------------------------------------------------------------------------
add_library(gekkopak STATIC
    src/gekkopak.cpp
)

target_include_directories(gekkopak
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(gekkopak PUBLIC cxx_std_17)
target_compile_options(gekkopak PRIVATE ${GEKKOPAK_WARNINGS})

add_executable(gekkopak_sim
    sim/main.cpp
)

target_link_libraries(gekkopak_sim PRIVATE gekkopak)

include(CTest)
if(BUILD_TESTING)
    # The test executables assert() their expectations. A Release build defines
    # NDEBUG, which would compile every one of them away and leave the suite
    # passing vacuously, so NDEBUG is removed from test targets specifically.
    if(MSVC)
        set(GEKKOPAK_TEST_ASSERTS /UNDEBUG)
    else()
        set(GEKKOPAK_TEST_ASSERTS -UNDEBUG)
    endif()
    add_executable(gekkopak_tests
        tests/test_simulator.cpp
    )
    target_link_libraries(gekkopak_tests PRIVATE gekkopak)
    add_test(NAME gekkopak_simulator COMMAND gekkopak_tests)

    add_executable(gekkopak_device_tests
        tests/test_device_core.cpp
    )
    target_link_libraries(gekkopak_device_tests PRIVATE gekkopak_core)
    target_compile_options(gekkopak_device_tests PRIVATE ${GEKKOPAK_WARNINGS}
        ${GEKKOPAK_TEST_ASSERTS})
    add_test(NAME gekkopak_device_core COMMAND gekkopak_device_tests)

    add_executable(gekkopak_transport_tests
        tests/test_ntr_transport.cpp
    )
    target_link_libraries(gekkopak_transport_tests PRIVATE gekkopak_core)
    target_compile_options(gekkopak_transport_tests PRIVATE ${GEKKOPAK_WARNINGS}
        ${GEKKOPAK_TEST_ASSERTS})
    add_test(NAME gekkopak_ntr_transport COMMAND gekkopak_transport_tests)

    # Golden wire vectors, replayed against every host-buildable target.
    add_executable(gekkopak_conformance
        tests/conformance/conformance_main.cpp
        tests/conformance/vector_runner.cpp
        tests/conformance/targets.cpp
        prototype/dspico-v1/hostshim/dspico_host_shim.cpp
        prototype/dspico-v1/overlay/src/gekkopakNtr.cpp
    )
    target_link_libraries(gekkopak_conformance PRIVATE gekkopak_core)
    target_include_directories(gekkopak_conformance PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/prototype/dspico-v1/hostshim
        ${CMAKE_CURRENT_SOURCE_DIR}/prototype/dspico-v1/overlay/src
    )
    target_compile_options(gekkopak_conformance PRIVATE ${GEKKOPAK_WARNINGS}
        ${GEKKOPAK_TEST_ASSERTS})
    target_compile_definitions(gekkopak_conformance PRIVATE
        GEKKOPAK_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/vectors")
    add_test(NAME gekkopak_conformance COMMAND gekkopak_conformance)

endif()
