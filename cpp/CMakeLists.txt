cmake_minimum_required(VERSION 3.21)
project(jabba VERSION 2.0.0 LANGUAGES CXX)

# Header-only: C++20 and the standard library, nothing else.
add_library(jabba INTERFACE)
add_library(jabba::jabba ALIAS jabba)
target_include_directories(jabba INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(jabba INTERFACE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(jabba INTERFACE Threads::Threads)

include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

option(JABBA_BUILD_TESTS "Build jabba's tests and example" ${PROJECT_IS_TOP_LEVEL})
if(JABBA_BUILD_TESTS)
    enable_testing()
    foreach(name conformance core)
        add_executable(jabba_${name} tests/${name}.cpp)
        target_link_libraries(jabba_${name} PRIVATE jabba::jabba)
        if(NOT MSVC)
            target_compile_options(jabba_${name} PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror)
        endif()
        add_test(NAME ${name} COMMAND jabba_${name})
    endforeach()
    # Tests read the shared cases from spec/ next to cpp/; JABBA_SPEC_DIR overrides it.
    target_compile_definitions(jabba_conformance PRIVATE
        JABBA_SPEC_DIR_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../spec")

    # Foreign nodes (ffi/): the C protocol kit, the testkit, and the tests through the ABI.
    enable_language(C)
    set(JABBA_FFI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ffi)
    add_library(jabba_ffi_testing STATIC ${JABBA_FFI_DIR}/testing/nodes.c ${JABBA_FFI_DIR}/testing/host.c)
    set_target_properties(jabba_ffi_testing PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
    target_include_directories(jabba_ffi_testing PUBLIC ${JABBA_FFI_DIR}/testing)
    target_link_libraries(jabba_ffi_testing PUBLIC Threads::Threads)

    # Shared, so that hosts in other languages can load them (Python via ctypes, ...).
    add_library(jabba_ffi_testing_shared SHARED ${JABBA_FFI_DIR}/testing/nodes.c ${JABBA_FFI_DIR}/testing/host.c)
    set_target_properties(jabba_ffi_testing_shared PROPERTIES C_STANDARD 11 OUTPUT_NAME jabba_ffi_testing)
    target_link_libraries(jabba_ffi_testing_shared PRIVATE Threads::Threads)
    add_library(jabba_testkit SHARED testkit/testkit.cpp)
    target_include_directories(jabba_testkit PUBLIC ${JABBA_FFI_DIR}/testing)
    target_link_libraries(jabba_testkit PRIVATE jabba::jabba)

    add_executable(jabba_ffi tests/ffi.cpp)
    target_link_libraries(jabba_ffi PRIVATE jabba::jabba jabba_ffi_testing)
    add_test(NAME ffi COMMAND jabba_ffi)

    # Another language's testkit (the same C API) instead of the C++ one: a path to its
    # shared or static library.
    set(JABBA_TESTKIT_LIBRARY "" CACHE FILEPATH "Testkit library for conformance_ffi (default: the C++ one)")
    add_executable(jabba_conformance_ffi tests/conformance.cpp)
    if(JABBA_TESTKIT_LIBRARY)
        target_include_directories(jabba_conformance_ffi PRIVATE ${JABBA_FFI_DIR}/testing)
        target_link_libraries(jabba_conformance_ffi PRIVATE jabba::jabba ${JABBA_TESTKIT_LIBRARY} ${CMAKE_DL_LIBS})
        if(NOT WIN32 AND NOT APPLE)
            target_link_libraries(jabba_conformance_ffi PRIVATE m)
        endif()
    else()
        target_link_libraries(jabba_conformance_ffi PRIVATE jabba::jabba jabba_testkit)
    endif()
    target_compile_definitions(jabba_conformance_ffi PRIVATE JABBA_CONFORMANCE_FOREIGN
        JABBA_SPEC_DIR_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../spec")
    add_test(NAME conformance_ffi COMMAND jabba_conformance_ffi)
    set_tests_properties(conformance_ffi PROPERTIES ENVIRONMENT "JABBA_FOREIGN=testkit")

    if(NOT MSVC)
        foreach(target jabba_ffi jabba_conformance_ffi jabba_testkit)
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror)
        endforeach()
        target_compile_options(jabba_ffi_testing PRIVATE -Wall -Wextra -Werror)
    endif()

    add_executable(jabba_example examples/greeting.cpp)
    target_link_libraries(jabba_example PRIVATE jabba::jabba)
    if(NOT MSVC)
        target_compile_options(jabba_example PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror)
    endif()
    add_test(NAME example COMMAND jabba_example)
endif()
