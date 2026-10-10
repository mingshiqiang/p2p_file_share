cmake_minimum_required(VERSION 3.22.1)
project(fileshare_native)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUICE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../../third_party/libjuice-1.7.4)
set(COMMON_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../../common)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_SERVER ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(USE_NETTLE OFF CACHE BOOL "" FORCE)
add_subdirectory(${JUICE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libjuice)

add_library(fileshare_native SHARED
    jni_link.cpp
    ${COMMON_DIR}/protocol/Crc16.cpp
    ${COMMON_DIR}/protocol/TimeUtil.cpp
    ${COMMON_DIR}/protocol/BleFramer.cpp
    ${COMMON_DIR}/protocol/SignalCodec.cpp
    ${COMMON_DIR}/protocol/FileName.cpp
    ${COMMON_DIR}/protocol/Sha256.cpp
    ${COMMON_DIR}/protocol/FileFrame.cpp
    ${COMMON_DIR}/protocol/FileTransferEngine.cpp
    ${COMMON_DIR}/p2p/JuiceSession.cpp
    ${COMMON_DIR}/p2p/LinkCore.cpp
)

target_include_directories(fileshare_native PRIVATE ${COMMON_DIR})
target_link_libraries(fileshare_native juice log)
target_compile_options(fileshare_native PRIVATE -Wall -Wno-unused-parameter)
