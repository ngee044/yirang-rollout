set(LIBRARY_NAME Health)

set(HEADER_FILES
    HealthTypes.h
    IHealthProbe.h
    HealthChecker.h
    ProcessProbe.h
    NetworkProbe.h
)

set(SOURCE_FILES
    HealthChecker.cpp
    ProcessProbe.cpp
    NetworkProbe.cpp
)

add_library(${LIBRARY_NAME} STATIC ${SOURCE_FILES} ${HEADER_FILES})

target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(CURL REQUIRED)

target_link_libraries(${LIBRARY_NAME} PUBLIC Process CURL::libcurl)
