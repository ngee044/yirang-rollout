cmake_minimum_required(VERSION 3.21)

set(PROGRAM_NAME "yirang-rollout")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(${PROGRAM_NAME} VERSION 0.1.0.0 LANGUAGES CXX)

option(BUILD_TESTS "Build unit tests (gtest)" ON)

# ------------------------------------------------------------------ platform --
# win / mac / ubuntu 3종을 지원한다. 컴파일러별 분기는 여기에만 둔다.
if (MSVC)
    add_compile_definitions(
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    add_compile_options(/utf-8 /MP /W3 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_DEBUG")
endif()

if (UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

find_package(Threads REQUIRED)

# ----------------------------------------------------------------- submodule --
# CppToolkit (git submodule) — 필수 의존. 현재 소비 모듈은 Utilities/ThreadPool뿐이므로
# 나머지는 빌드하지 않는다. 소비 시점에 해당 모듈만 ON으로 전환한다.
set(BUILD_DATABASE_LIB OFF CACHE BOOL "Build database library")
set(BUILD_NETWORK_LIB OFF CACHE BOOL "Build network library")
set(BUILD_REDIS_LIB OFF CACHE BOOL "Build redis library")
set(BUILD_RABBIT_MQ_LIB OFF CACHE BOOL "Build rabbitmq library")
set(BUILD_KAFKA_LIB OFF CACHE BOOL "Build kafka library")
set(BUILD_SAMPLES OFF CACHE BOOL "Build samples")
add_subdirectory(.cpptoolkit)

# -------------------------------------------------------------------- targets --
add_subdirectory(Main)

if (BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
