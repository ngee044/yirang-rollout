set(TEST_NAME ${PROGRAM_NAME}Test)

set(TEST_SOURCE_FILES
    SampleTest.cpp
)

find_package(GTest CONFIG REQUIRED)

add_executable(${TEST_NAME} ${TEST_SOURCE_FILES})

target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/Main)

# GTest::gtest_main 이 GTest::gtest 를 전이적으로 링크한다.
# 둘을 함께 지정하면 링커가 중복 라이브러리 경고를 낸다.
target_link_libraries(${TEST_NAME} PRIVATE GTest::gtest_main Threads::Threads Utilities Thread)

include(GoogleTest)
gtest_discover_tests(${TEST_NAME})
