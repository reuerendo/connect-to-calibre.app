#!/bin/bash

# Configuration script for Calibre Companion

set -e

echo "Configuring Calibre Companion for PocketBook InkPad 4..."

# Check if SDK exists
if [ ! -d "SDK/SDK_6.3.0" ]; then
    echo "PocketBook SDK not found. Downloading..."
    mkdir -p SDK
    cd SDK
    git clone --depth 1 --branch 5.19 https://github.com/pocketbook/SDK_6.3.0.git
    cd ..
    echo "SDK downloaded successfully."
fi

# Create build directory
if [ ! -d "build" ]; then
    mkdir build
    echo "Build directory created."
fi

# Configure CMake
echo "Configuring CMake..."
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake "$@"

echo ""
echo "Configuration complete!"
echo "To build the project, run:"
echo "  cmake --build ./build"
echo ""
echo "The compiled application will be available at:"
echo "  build/connect-to-calibre.app"
