# 소스는 명시적으로 나열한다. file(GLOB)은 사용하지 않는다.
# GLOB은 파일 추가/삭제가 재구성에 반영되지 않고, 빌드에서 누락된 소스를 검출할 수 없다.
set(SOURCE_FILES
    Configurations.cpp
    main.cpp
)

set(HEADER_FILES
    Configurations.h
)

add_executable(${PROGRAM_NAME} ${SOURCE_FILES} ${HEADER_FILES})

target_include_directories(${PROGRAM_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Boost REQUIRED COMPONENTS json)

# CppToolkit 링크 타겟명은 디렉터리명과 다를 수 있다 (ThreadPool/ -> Thread).
target_link_libraries(${PROGRAM_NAME} PRIVATE Threads::Threads Boost::json Utilities Thread)

# 실행 파일 옆에 설정 파일을 배치한다 (Configurations가 program_folder 기준으로 탐색).
add_custom_command(TARGET ${PROGRAM_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Configurations.json"
        "$<TARGET_FILE_DIR:${PROGRAM_NAME}>/Configurations.json"
)
