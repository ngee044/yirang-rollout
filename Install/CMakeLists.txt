set(LIBRARY_NAME Install)

project(${LIBRARY_NAME} VERSION 0.1.0 LANGUAGES CXX)

set(HEADER_FILES
    InstallTypes.h
    ReleaseInstaller.h
)

set(SOURCE_FILES
    ReleaseInstaller.cpp
)

add_library(${LIBRARY_NAME} STATIC ${SOURCE_FILES} ${HEADER_FILES})

target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Boost REQUIRED COMPONENTS json)

target_link_libraries(${LIBRARY_NAME} PUBLIC Boost::json)
