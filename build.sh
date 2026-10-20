#!/bin/bash

set -e

echo "Building Calibre Companion for PocketBook InkPad 4 (Static build)..."

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

SDK_VERSION="6.10"
SDK_DIR="SDK/SDK_6.10"
SDK_ARCHIVE="SDK-B288-6.10.7z"
SDK_URL="https://github.com/Sean-on-Git/PocketBook-SDK/releases/download/6.10/${SDK_ARCHIVE}"

# Check if SDK exists
if [ ! -d "${SDK_DIR}/SDK-B288" ] && [ ! -d "SDK/SDK_6.3.0/SDK-B288" ]; then
    echo -e "${YELLOW}PocketBook SDK ${SDK_VERSION} not found. Downloading...${NC}"
    mkdir -p SDK
    cd SDK
    
    # Download SDK archive
    echo -e "${YELLOW}Downloading ${SDK_ARCHIVE}...${NC}"
    curl -L -o "${SDK_ARCHIVE}" "${SDK_URL}"
    
    # Extract using bsdtar
    echo -e "${YELLOW}Extracting SDK...${NC}"
    if ! command -v bsdtar &> /dev/null; then
        echo -e "${RED}Error: bsdtar not found. Please install libarchive-tools:${NC}"
        echo "  sudo apt-get install libarchive-tools"
        exit 1
    fi
    
    mkdir -p SDK_6.10
    bsdtar -xf "${SDK_ARCHIVE}" -C SDK_6.10
    
    # Clean up archive
    rm "${SDK_ARCHIVE}"
    
    if [ ! -d "SDK_6.10/SDK-B288" ] && [ -d "SDK_6.10/usr" ]; then
        mkdir -p SDK_6.10_tmp
        mv SDK_6.10 SDK_6.10_tmp/SDK-B288
        mkdir -p SDK_6.10
        mv SDK_6.10_tmp/SDK-B288 SDK_6.10/
        rm -rf SDK_6.10_tmp
    fi
    
    cd ..
    echo -e "${GREEN}SDK ${SDK_VERSION} downloaded and extracted successfully.${NC}"
fi

# Clean previous build
echo -e "${YELLOW}Cleaning previous build...${NC}"
rm -rf build

# Create build directory
mkdir build
echo -e "${GREEN}Build directory created.${NC}"

# Configure CMake
echo -e "${YELLOW}Configuring CMake for static build...${NC}"
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DTOOLCHAIN_PATH="${PWD}/../SDK/SDK_6.10/SDK-B288" "$@"

# Build
echo -e "${YELLOW}Building with static libraries...${NC}"
cmake --build . --config Release -- -j$(nproc)

# Check binary
echo ""
echo -e "${YELLOW}Binary information:${NC}"
ls -lh connect-to-calibre.app
file connect-to-calibre.app

# Check dependencies
echo ""
echo -e "${YELLOW}Checking dynamic dependencies:${NC}"
READELF_BIN=""
if [ -f "../SDK/SDK_6.10/SDK-B288/usr/bin/arm-obreey-linux-gnueabi-readelf" ]; then
    READELF_BIN="../SDK/SDK_6.10/SDK-B288/usr/bin/arm-obreey-linux-gnueabi-readelf"
elif [ -f "../SDK/SDK_6.3.0/SDK-B288/usr/bin/arm-obreey-linux-gnueabi-readelf" ]; then
    READELF_BIN="../SDK/SDK_6.3.0/SDK-B288/usr/bin/arm-obreey-linux-gnueabi-readelf"
fi

if [ -n "${READELF_BIN}" ]; then
    ${READELF_BIN} -d connect-to-calibre.app | grep NEEDED || echo -e "${GREEN}No dynamic dependencies found (fully static)${NC}"
fi

echo ""
echo -e "${GREEN}Build complete!${NC}"
echo -e "Output: ${GREEN}build/connect-to-calibre.app${NC}"
echo ""
echo "To install on device:"
echo "  1. Copy connect-to-calibre.app to /applications on device"
echo "  2. Restart device or open Applications menu"
echo ""
echo -e "${YELLOW}Note: Static build is larger but independent of system libraries${NC}"
