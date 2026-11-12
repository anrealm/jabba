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

    add_executable(jabba_example examples/greeting.cpp)
    target_link_libraries(jabba_example PRIVATE jabba::jabba)
    if(NOT MSVC)
        target_compile_options(jabba_example PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror)
    endif()
    add_test(NAME example COMMAND jabba_example)
endif()
