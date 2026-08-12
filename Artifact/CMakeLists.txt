set(LIBRARY_NAME Artifact)

set(HEADER_FILES
    ArtifactTypes.h
    IArtifactStore.h
    ArtifactKey.h
    S3ArtifactStore.h
)

set(SOURCE_FILES
    ArtifactKey.cpp
    S3ArtifactStore.cpp
)

add_library(${LIBRARY_NAME} STATIC ${SOURCE_FILES} ${HEADER_FILES})

target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${LIBRARY_NAME} PUBLIC AWSService)
