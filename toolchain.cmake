# Toolchain file for cross-compiling to PocketBook InkPad 4

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Specify the cross compiler
set(TOOLCHAIN_PATH "${CMAKE_SOURCE_DIR}/SDK/SDK_6.3.0/SDK-B288" CACHE PATH "Path to PocketBook SDK")

set(CMAKE_C_COMPILER ${TOOLCHAIN_PATH}/usr/bin/arm-obreey-linux-gnueabi-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PATH}/usr/bin/arm-obreey-linux-gnueabi-g++)
set(CMAKE_STRIP ${TOOLCHAIN_PATH}/usr/bin/arm-obreey-linux-gnueabi-strip)

# Where is the target environment
set(CMAKE_FIND_ROOT_PATH ${TOOLCHAIN_PATH}/usr/arm-obreey-linux-gnueabi/sysroot)

# Search for programs in the build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# For libraries and headers in the target directories
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Prefer static libraries
set(CMAKE_FIND_LIBRARY_SUFFIXES .a .so)

# Compiler flags for static linking
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -static-libgcc")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -static-libgcc -static-libstdc++")

# Linker flags for static linking
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++ -Wl,--as-needed")

# Optional: Full static linking (uncomment if needed)
# set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
# set(BUILD_SHARED_LIBS OFF)
