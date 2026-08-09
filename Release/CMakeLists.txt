# 소스는 명시적으로 나열한다. file(GLOB)은 사용하지 않는다.
set(LIBRARY_NAME Release)

set(HEADER_FILES
    ReleaseTypes.h
    ReleaseManifest.h
)

set(SOURCE_FILES
    ReleaseManifest.cpp
)

add_library(${LIBRARY_NAME} STATIC ${SOURCE_FILES} ${HEADER_FILES})

# flat include 규약: 소비 측은 #include "ReleaseManifest.h" 로 포함한다.
target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# vcpkg 패키지명(openssl) ≠ find_package 이름(OpenSSL) ≠ 링크 타겟(OpenSSL::Crypto).
# SHA-256 만 쓰므로 Crypto 만 링크한다 (TLS 는 불필요).
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)

target_link_libraries(${LIBRARY_NAME} PUBLIC OpenSSL::Crypto Boost::json)
