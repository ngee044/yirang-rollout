set(TEST_NAME ${PROGRAM_NAME}Test)

set(TEST_SOURCE_FILES
    ConfigurationsTest.cpp
    ProcessSupervisorTest.cpp
    HealthCheckerTest.cpp
    ReleaseManifestTest.cpp
    ArtifactStoreTest.cpp
    AgentServiceTest.cpp
    MessagingTest.cpp
    DeployCliTest.cpp
    ReleaseInstallerTest.cpp
    DeploymentEngineTest.cpp
)

find_package(GTest CONFIG REQUIRED)

add_executable(${TEST_NAME} ${TEST_SOURCE_FILES})

target_link_libraries(${TEST_NAME} PRIVATE GTest::gtest_main Threads::Threads YirangAgentCore DeployCLICore Deploy Install Process Health Release Artifact Messaging)

include(GoogleTest)
gtest_discover_tests(${TEST_NAME})
