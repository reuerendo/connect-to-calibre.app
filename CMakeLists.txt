cmake_minimum_required(VERSION 3.10)
project(connect-to-calibre)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set source encoding
add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)

# PocketBook SDK path
set(TOOLCHAIN_PATH "${CMAKE_SOURCE_DIR}/SDK/SDK_6.3.0/SDK-B288" CACHE PATH "Path to PocketBook SDK")

# Set the toolchain for cross-compilation
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/toolchain.cmake")
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${TOOLCHAIN_PATH}/usr/arm-obreey-linux-gnueabi/sysroot/usr/include
    ${TOOLCHAIN_PATH}/usr/arm-obreey-linux-gnueabi/sysroot/usr/include/freetype2
    ${TOOLCHAIN_PATH}/usr/arm-obreey-linux-gnueabi/sysroot/usr/include/json-c
)

# Link directories
link_directories(
    ${TOOLCHAIN_PATH}/usr/arm-obreey-linux-gnueabi/sysroot/usr/lib
)

# Source files
set(SOURCES
    src/main.cpp
    src/network.cpp
    src/calibre_protocol.cpp
    src/book_manager.cpp
    src/cache_manager.cpp
    src/i18n.cpp
)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Force static linking for all libraries
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")

# Try to find static libraries first
set(CMAKE_FIND_LIBRARY_SUFFIXES .a .so)

# Link libraries in correct order (dependencies matter for static linking)
# Order: high-level libs first, then their dependencies
target_link_libraries(${PROJECT_NAME}
    -Wl,--start-group
    # Main libraries
    -static inkview
    -static freetype
    -static json-c
    -static sqlite3
    -static ssl
    -static crypto
    -static z
    -static pthread
    # Additional dependencies that might be needed
    -static png
    -static bz2
    -static dl
    -static rt
    -static m
    -Wl,--end-group
)

# Strip the binary to reduce size
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:${PROJECT_NAME}>
    COMMENT "Stripping binary to reduce size"
)

# Set output name
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "connect-to-calibre.app")

# Installation
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION .
)
