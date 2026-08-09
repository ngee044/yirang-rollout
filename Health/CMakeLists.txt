# 소스는 명시적으로 나열한다. file(GLOB)은 사용하지 않는다.
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

# flat include 규약: 소비 측은 #include "HealthChecker.h" 로 포함한다.
target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# vcpkg 패키지명(curl) ≠ find_package 이름(CURL) ≠ 링크 타겟(CURL::libcurl).
find_package(CURL REQUIRED)

# Process: ProcessProbe 가 IProcessSupervisor::status 로 프로세스 생존을 확인한다.
target_link_libraries(${LIBRARY_NAME} PUBLIC Process CURL::libcurl)
