@echo off
rem
rem yirang-rollout build script for Windows (CMake + vcpkg).
rem
rem Usage:
rem   scripts\build.bat [Release^|Debug] [--clean]
rem
rem Environment overrides:
rem   VCPKG_ROOT   vcpkg checkout path (default: %USERPROFILE%\vcpkg)
rem
setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
cd /d "%SCRIPT_DIR%.."
set PROJECT_DIR=%CD%

set BUILD_TYPE=Release
set DO_CLEAN=0

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="Debug" (set BUILD_TYPE=Debug& shift & goto parse)
if /i "%~1"=="Release" (set BUILD_TYPE=Release& shift & goto parse)
if /i "%~1"=="--clean" (set DO_CLEAN=1& shift & goto parse)
echo [build.bat] unknown option: %~1
exit /b 1
:parsed

if "%VCPKG_ROOT%"=="" set VCPKG_ROOT=%USERPROFILE%\vcpkg
set TOOLCHAIN=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake

if not exist "%TOOLCHAIN%" (
	echo [build.bat] vcpkg toolchain not found: %TOOLCHAIN%
	echo [build.bat] VCPKG_ROOT 환경변수를 지정하십시오.
	exit /b 1
)

if exist "%PROJECT_DIR%\.gitmodules" (
	git submodule update --init --recursive
	if errorlevel 1 exit /b 1
)

if "%DO_CLEAN%"=="1" (
	echo [build.bat] removing build
	if exist build rmdir /s /q build
)

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DBUILD_SHARED_LIBS=OFF
if errorlevel 1 exit /b 1

cmake --build build --config %BUILD_TYPE% --parallel
if errorlevel 1 exit /b 1

echo [build.bat] 완료. 실행 파일: build\out (multi-config 생성기는 build\out\%BUILD_TYPE%)
echo [build.bat] 테스트: cd build ^&^& ctest -C %BUILD_TYPE% --output-on-failure

endlocal
