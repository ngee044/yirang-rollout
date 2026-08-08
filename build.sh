#!/bin/bash
#
# yirang-rollout build script (CMake + Ninja + vcpkg). macOS / Linux.
# Windows 는 scripts/build.bat 을 사용한다.
#
# Usage:
#   ./build.sh [--debug|--release] [--clean] [--target <name>] [-j N]
#
# Environment overrides:
#   VCPKG_ROOT   vcpkg 체크아웃 경로 (default: $HOME/vcpkg)
#   BUILD_TYPE   Release | Debug (default: Release)
#   SDKROOT      macOS SDK 강제 지정 (libc++ 헤더 문제 발생 시)
#   CXX          컴파일러
#
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="$SCRIPT_DIR/build"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
CLEAN=0
TARGET=""
JOBS=""

while [ $# -gt 0 ]; do
	case "$1" in
		--debug) BUILD_TYPE="Debug"; shift ;;
		--release) BUILD_TYPE="Release"; shift ;;
		--clean) CLEAN=1; shift ;;
		--target) TARGET="${2:?--target requires a name}"; shift 2 ;;
		-j) JOBS="${2:?-j requires a number}"; shift 2 ;;
		-j*) JOBS="${1#-j}"; shift ;;
		-h|--help) sed -n '2,17p' "$0"; exit 0 ;;
		*) echo "[build.sh] unknown option: $1" >&2; exit 1 ;;
	esac
done

TOOLCHAIN="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
if [ ! -f "$TOOLCHAIN" ]; then
	echo "[build.sh] vcpkg toolchain not found: $TOOLCHAIN" >&2
	echo "[build.sh] VCPKG_ROOT 를 지정하거나 vcpkg 를 $HOME/vcpkg 에 clone 하십시오." >&2
	exit 1
fi

# 서브모듈이 초기화되지 않은 상태로는 빌드가 실패한다.
if [ -f "$SCRIPT_DIR/.gitmodules" ] && git submodule status --recursive 2>/dev/null | grep -q '^-'; then
	echo "[build.sh] 초기화되지 않은 서브모듈 발견 -> git submodule update --init --recursive"
	git submodule update --init --recursive
fi

GENERATOR="Ninja"
if ! command -v ninja >/dev/null 2>&1; then
	echo "[build.sh] ninja 를 찾지 못했습니다. 기본 생성기를 사용합니다."
	GENERATOR=""
fi

# --- macOS: 활성 컴파일러가 실제로 파싱할 수 있는 SDK 선택 -------------------
# 최신 CommandLineTools SDK 의 libc++ 헤더가 설치된 clang 에 없는 빌트인
# (__builtin_ctzg 등)을 사용하는 경우가 있다. 이 상태에서는 프로젝트 빌드는 물론
# vcpkg 포트 소스 빌드(gtest, boost-context 등)까지 실패한다.
# 기본 SDK 를 프로브해 실패하면 컴파일 가능한 SDK 를 찾아 사용하고, 포트 빌드에도
# 같은 SDK 가 쓰이도록 SDKROOT 를 export 한다 (custom-triplets 가 이를 읽는다).
if [ "$(uname -s)" = "Darwin" ] && [ -z "${SDKROOT:-}" ]; then
	CXX_PROBE="${CXX:-c++}"
	probe_sdk() {
		local sdk="$1"
		local src='#include <bitset>
#include <string>
int main(){}'
		if [ -n "$sdk" ]; then
			printf '%s\n' "$src" | "$CXX_PROBE" -std=gnu++23 -isysroot "$sdk" -x c++ -c - -o /dev/null >/dev/null 2>&1
		else
			printf '%s\n' "$src" | "$CXX_PROBE" -std=gnu++23 -x c++ -c - -o /dev/null >/dev/null 2>&1
		fi
	}
	if ! probe_sdk ""; then
		for sdk in \
			/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX*.sdk \
			/Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk; do
			[ -d "$sdk" ] || continue
			if probe_sdk "$sdk"; then
				echo "[build.sh] 기본 SDK 가 '$CXX_PROBE' 와 호환되지 않습니다. 사용할 SDK: $sdk"
				export SDKROOT="$sdk"
				break
			fi
		done
		if [ -z "${SDKROOT:-}" ]; then
			echo "[build.sh] WARNING: 호환 가능한 macOS SDK 를 찾지 못했습니다. 기본 SDK 로 진행합니다." >&2
		fi
	fi
fi

if [ "$CLEAN" = "1" ]; then
	echo "[build.sh] removing $BUILD_DIR"
	rm -rf "$BUILD_DIR"
fi

CMAKE_ARGS=(
	-S "$SCRIPT_DIR" -B "$BUILD_DIR"
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
	-DCMAKE_BUILD_TYPE="$BUILD_TYPE"
	-DBUILD_SHARED_LIBS=OFF
)
[ -n "$GENERATOR" ] && CMAKE_ARGS+=( -G "$GENERATOR" )
[ -n "${SDKROOT:-}" ] && CMAKE_ARGS+=( -DCMAKE_OSX_SYSROOT="$SDKROOT" )
[ -d "$SCRIPT_DIR/custom-triplets" ] && CMAKE_ARGS+=( -DVCPKG_OVERLAY_TRIPLETS="$SCRIPT_DIR/custom-triplets" )

cmake "${CMAKE_ARGS[@]}"

BUILD_ARGS=( --build "$BUILD_DIR" --config "$BUILD_TYPE" )
[ -n "$TARGET" ] && BUILD_ARGS+=( --target "$TARGET" )
if [ -n "$JOBS" ]; then
	BUILD_ARGS+=( -j "$JOBS" )
else
	BUILD_ARGS+=( -j )
fi

cmake "${BUILD_ARGS[@]}"

echo "[build.sh] 완료. 실행 파일: build/out, 라이브러리: build/lib"
echo "[build.sh] 테스트: cd build && ctest --output-on-failure"
