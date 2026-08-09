# 소스는 명시적으로 나열한다. file(GLOB)은 사용하지 않는다.
set(LIBRARY_NAME Messaging)

set(HEADER_FILES
    MessagingTypes.h
    IMessageConsumer.h
    SqsMessageConsumer.h
    IMessagePublisher.h
    SqsMessagePublisher.h
)

set(SOURCE_FILES
    SqsMessageConsumer.cpp
    SqsMessagePublisher.cpp
)

add_library(${LIBRARY_NAME} STATIC ${SOURCE_FILES} ${HEADER_FILES})

# flat include 규약: 소비 측은 #include "SqsMessageConsumer.h" 로 포함한다.
target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# CppToolkit AWSService: 폴더명 = 타겟명 = 네임스페이스가 모두 AWSService 다.
# aws-sdk-cpp 는 AWSService 가 PUBLIC 으로 전파한다.
target_link_libraries(${LIBRARY_NAME} PUBLIC AWSService)
