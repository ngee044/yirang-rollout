set(LIBRARY_NAME Process)

set(HEADER_FILES
    ProcessTypes.h
    IProcessSupervisor.h
)

set(SOURCE_FILES "")

if (WIN32)
    message(FATAL_ERROR
        "Process 모듈의 Windows 구현(WindowsProcessSupervisor)이 아직 없습니다. "
        "CreateProcessW·Job Objects 기반 구현은 T-005 후속 작업입니다 (아키텍처 §17.2).")
else()
    list(APPEND HEADER_FILES PosixProcessSupervisor.h)
    list(APPEND SOURCE_FILES PosixProcessSupervisor.cpp)
endif()

add_library(${LIBRARY_NAME} STATIC ${SOURCE_FILES} ${HEADER_FILES})

target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${LIBRARY_NAME} PUBLIC Threads::Threads)
