set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

# build.sh 가 활성 컴파일러로 파싱 가능한 SDK 를 찾아 SDKROOT 로 export 한다.
# vcpkg 포트 *소스 빌드* 도 같은 SDK 를 쓰도록 여기서 받아 넘긴다.
# (최신 CommandLineTools SDK 의 libc++ 헤더는 설치된 clang 에 없는 빌트인을
#  사용하는 경우가 있어 gtest, boost-context 등의 포트 빌드가 실패한다.)
if(DEFINED ENV{SDKROOT} AND NOT "$ENV{SDKROOT}" STREQUAL "")
	set(VCPKG_OSX_SYSROOT "$ENV{SDKROOT}")
endif()
