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

target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${LIBRARY_NAME} PUBLIC AWSService)
