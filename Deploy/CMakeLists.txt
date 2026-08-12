set(LIBRARY_NAME Deploy)

project(${LIBRARY_NAME} VERSION 0.1.0 LANGUAGES CXX)

set(HEADER_FILES
    DeployTypes.h
    DeploymentEngine.h
)

set(SOURCE_FILES
    DeploymentEngine.cpp
)

add_library(${LIBRARY_NAME} STATIC ${SOURCE_FILES} ${HEADER_FILES})

target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${LIBRARY_NAME} PUBLIC Install Process Health)
