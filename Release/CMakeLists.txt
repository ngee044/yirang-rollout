set(LIBRARY_NAME Release)

set(HEADER_FILES
    ReleaseTypes.h
    ReleaseManifest.h
)

set(SOURCE_FILES
    ReleaseManifest.cpp
)

add_library(${LIBRARY_NAME} STATIC ${SOURCE_FILES} ${HEADER_FILES})

target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)

target_link_libraries(${LIBRARY_NAME} PUBLIC OpenSSL::Crypto Boost::json)
