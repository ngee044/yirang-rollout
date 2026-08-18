set(EXECUTABLE_NAME DeployCLI)

project(${EXECUTABLE_NAME} VERSION 0.1.0 LANGUAGES CXX)

set(CORE_LIBRARY_NAME DeployCLICore)

set(CORE_HEADER_FILES
    Configurations.h
    Confirmation.h
    RestClient.h
    Commands.h
)

set(CORE_SOURCE_FILES
    Configurations.cpp
    Confirmation.cpp
    RestClient.cpp
    Commands.cpp
)

add_library(${CORE_LIBRARY_NAME} STATIC ${CORE_HEADER_FILES} ${CORE_SOURCE_FILES})

target_include_directories(${CORE_LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Boost REQUIRED COMPONENTS json)

find_package(CURL REQUIRED)

target_link_libraries(${CORE_LIBRARY_NAME} PUBLIC Utilities Boost::json Artifact Release CURL::libcurl)

set(SOURCE_FILES
    main.cpp
)

add_executable(${EXECUTABLE_NAME} ${SOURCE_FILES})

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
