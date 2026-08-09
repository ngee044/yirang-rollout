# 소스는 명시적으로 나열한다. file(GLOB)은 사용하지 않는다.
set(EXECUTABLE_NAME DeployCLI)

project(${EXECUTABLE_NAME} VERSION 0.1.0 LANGUAGES CXX)

# 실행 진입점(main.cpp)을 제외한 전부를 정적 라이브러리로 두어 테스트가 같은 오브젝트를 링크한다.
set(CORE_LIBRARY_NAME DeployCLICore)

set(CORE_HEADER_FILES
    Configurations.h
)

set(CORE_SOURCE_FILES
    Configurations.cpp
)

add_library(${CORE_LIBRARY_NAME} STATIC ${CORE_HEADER_FILES} ${CORE_SOURCE_FILES})

# flat include 규약: 소비 측은 #include "Configurations.h" 로 포함한다.
target_include_directories(${CORE_LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Boost REQUIRED COMPONENTS json)

# CppToolkit 링크 타겟명은 디렉터리명과 다를 수 있다. Utilities 가 ArgumentParser/File/Converter/Logger 를 제공한다.
target_link_libraries(${CORE_LIBRARY_NAME} PUBLIC Utilities Boost::json)

set(SOURCE_FILES
    main.cpp
)

add_executable(${EXECUTABLE_NAME} ${SOURCE_FILES})

# 타겟명(DeployCLI)과 배포되는 실행 파일명(yirang)은 다르다 (아키텍처 §34).
set_target_properties(${EXECUTABLE_NAME} PROPERTIES OUTPUT_NAME yirang)

target_link_libraries(${EXECUTABLE_NAME} PRIVATE Threads::Threads ${CORE_LIBRARY_NAME})
target_compile_definitions(${EXECUTABLE_NAME} PRIVATE YIRANG_DEPLOY_CLI_VERSION="${PROJECT_VERSION}")

set(JSON_FILES
    yirang_deploy_configurations.json
)

foreach(JSON_FILE IN LISTS JSON_FILES)
    add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/${JSON_FILE}"
            "$<TARGET_FILE_DIR:${EXECUTABLE_NAME}>/${JSON_FILE}"
    )
endforeach()
