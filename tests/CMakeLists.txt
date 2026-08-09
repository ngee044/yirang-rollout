set(TEST_NAME ${PROGRAM_NAME}Test)

set(TEST_SOURCE_FILES
    ConfigurationsTest.cpp
    ProcessSupervisorTest.cpp
    HealthCheckerTest.cpp
    ReleaseManifestTest.cpp
    ArtifactStoreTest.cpp
    AgentServiceTest.cpp
    MessagingTest.cpp
)

find_package(GTest CONFIG REQUIRED)

add_executable(${TEST_NAME} ${TEST_SOURCE_FILES})

# GTest::gtest_main 이 GTest::gtest 를 전이적으로 링크한다.
# 둘을 함께 지정하면 링커가 중복 라이브러리 경고를 낸다.
# YirangAgentCore 가 Configurations 의 include 경로와 Utilities·Boost::json 을 PUBLIC 으로 전파한다.
target_link_libraries(${TEST_NAME} PRIVATE GTest::gtest_main Threads::Threads YirangAgentCore Process Health Release Artifact Messaging)

include(GoogleTest)
gtest_discover_tests(${TEST_NAME})
