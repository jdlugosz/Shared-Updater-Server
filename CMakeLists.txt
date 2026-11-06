cmake_minimum_required(VERSION 3.30)
project(SharedUpdaterServer VERSION 0.1.0 LANGUAGES CXX)

set(UPDATER_SANITIZER "none" CACHE STRING "none, address, or thread")
set(UPDATER_BUILD_VARIANT "normal" CACHE STRING "Build label, such as profile or special")
option(UPDATER_TRUST_SHARED_CHECKOUT "Allow Git metadata reads from the mounted staging checkout" OFF)
option(UPDATER_USE_STD_MODULE "Use the C++ standard library module in implementation files" OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ${UPDATER_USE_STD_MODULE})
if(UPDATER_USE_STD_MODULE)
    if(NOT 26 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
        message(FATAL_ERROR "The C++26 std module is unavailable. Use a fresh build directory with a supported toolchain and CMake import std enabled.")
    endif()
    set(CMAKE_CXX_MODULE_STD ON)
    add_compile_definitions(UPDATER_USE_STD_MODULE)
endif()

if(NOT UPDATER_BUILD_VARIANT MATCHES "^[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR "UPDATER_BUILD_VARIANT must be a nonempty label with no spaces")
endif()
if(UPDATER_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
    add_link_options(-fsanitize=address,undefined -no-pie)
elseif(UPDATER_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -fno-pie)
    add_link_options(-fsanitize=thread -no-pie)
elseif(NOT UPDATER_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "Unknown UPDATER_SANITIZER")
endif()

find_package(tomlplusplus 3.4 CONFIG REQUIRED)

add_library(updater-catalog STATIC src/catalog.cpp src/binary.cpp src/xml.cpp src/uri.cpp)
target_include_directories(updater-catalog PUBLIC include)
target_compile_features(updater-catalog PUBLIC cxx_std_26)
target_link_libraries(updater-catalog PRIVATE tomlplusplus::tomlplusplus)
set_target_properties(updater-catalog PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(updater-catalog PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Git REQUIRED)
else()
    find_package(Git QUIET)
endif()

find_package(LibXml2 REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
add_library(updater-core STATIC src/request.cpp src/atom.cpp src/contact_log.cpp src/census.cpp src/processor.cpp src/worker.cpp src/json.cpp)
target_link_libraries(updater-core PUBLIC updater-catalog PRIVATE LibXml2::LibXml2 Threads::Threads SQLite::SQLite3)
target_compile_features(updater-core PUBLIC cxx_std_26)
set_target_properties(updater-core PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(updater-core PRIVATE -Wall -Wextra -Wpedantic)

find_package(Boost 1.83 CONFIG REQUIRED)
add_library(updater-http STATIC src/http_server.cpp src/config.cpp)
target_link_libraries(updater-http PUBLIC updater-core Boost::headers PRIVATE tomlplusplus::tomlplusplus)
target_compile_features(updater-http PUBLIC cxx_std_26)
set_target_properties(updater-http PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(updater-http PRIVATE -Wall -Wextra -Wpedantic)

find_library(SYSTEMD_LIBRARY systemd REQUIRED)
add_executable(shared-updater-server src/main.cpp src/service.cpp)
target_link_libraries(shared-updater-server PRIVATE updater-http ${SYSTEMD_LIBRARY})
target_compile_features(shared-updater-server PRIVATE cxx_std_26)
set_target_properties(shared-updater-server PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(shared-updater-server PRIVATE -Wall -Wextra -Wpedantic)

set(build_info_source "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/build_info.cpp")
add_custom_target(updater-build-info
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DTRUST_SHARED_CHECKOUT=${UPDATER_TRUST_SHARED_CHECKOUT}"
        "-DPROJECT_VERSION=${PROJECT_VERSION}"
        "-DBUILD_CONFIG=$<CONFIG>"
        "-DBUILD_VARIANT=${UPDATER_BUILD_VARIANT}"
        "-DBUILD_SANITIZER=${UPDATER_SANITIZER}"
        "-DOUTPUT_FILE=${build_info_source}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake"
    BYPRODUCTS "${build_info_source}"
    VERBATIM)
add_dependencies(shared-updater-server updater-build-info)
target_sources(shared-updater-server PRIVATE "${build_info_source}")

install(TARGETS shared-updater-server RUNTIME DESTINATION libexec)
install(PROGRAMS tools/census.py tools/publish_catalog.py DESTINATION libexec/shared-updater)
install(FILES deploy/shared-updater-server.service DESTINATION lib/systemd/system)
install(DIRECTORY examples/ DESTINATION share/shared-updater/examples)

if(BUILD_TESTING)
    find_package(SQLite3 REQUIRED)
    find_package(Catch2 3 CONFIG REQUIRED)

    add_executable(updater-server-tests tests/test_main.cpp tests/catalog_tests.cpp tests/feed_tests.cpp tests/persistence_tests.cpp tests/worker_tests.cpp tests/config_tests.cpp)
    target_compile_features(updater-server-tests PRIVATE cxx_std_26)
    set_target_properties(updater-server-tests PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(updater-server-tests PRIVATE
        updater-catalog
        updater-core
        updater-http
        LibXml2::LibXml2
        SQLite::SQLite3
        tomlplusplus::tomlplusplus
        Catch2::Catch2WithMain
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(updater-server-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    include(Catch)
    catch_discover_tests(updater-server-tests)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME updater.version
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/version_integration.py"
            "$<TARGET_FILE:shared-updater-server>" "${build_info_source}" "${CMAKE_COMMAND}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake" "${GIT_EXECUTABLE}")
    set_tests_properties(updater.version PROPERTIES TIMEOUT 30 LABELS "integration;build")
    add_test(NAME updater.http COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/server_integration.py
        --server $<TARGET_FILE:shared-updater-server> --mode http)
    set_tests_properties(updater.http PROPERTIES TIMEOUT 45 LABELS "integration;http")
    foreach(operation IN ITEMS operations publication)
        add_test(NAME updater.${operation} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/operator_integration.py
            --server $<TARGET_FILE:shared-updater-server> --tools ${CMAKE_CURRENT_SOURCE_DIR}/tools
            --mode ${operation})
        set_tests_properties(updater.${operation} PROPERTIES TIMEOUT 45 LABELS "integration;operators")
    endforeach()
    find_program(CADDY_EXECUTABLE caddy)
    if(CADDY_EXECUTABLE)
        add_test(NAME updater.caddy COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/server_integration.py
            --server $<TARGET_FILE:shared-updater-server> --mode proxy --caddy ${CADDY_EXECUTABLE})
        set_tests_properties(updater.caddy PROPERTIES TIMEOUT 45 LABELS "integration;caddy")
    endif()
endif()
