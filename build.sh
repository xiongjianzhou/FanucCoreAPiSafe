#!/bin/bash
# FOCAS2 Web API Server - Linux Build Script
# Requires: g++-multilib, libc6:i386, libstdc++6:i386

set -e
echo "Building FOCAS2 Web API Server (Linux)..."

if [ ! -f "NativeLib/Linux/libfwlib32.so" ]; then
    echo "ERROR: NativeLib/Linux/libfwlib32.so not found"
    exit 1
fi

g++ -m32 -o focaswebapi focaswebapi.c \
    -I./NativeLib/Linux \
    -L./NativeLib/Linux \
    -lfwlib32 -lstdc++ -lpthread

echo ""
echo "Build succeeded: focaswebapi"
echo "Run: LD_LIBRARY_PATH=./NativeLib/Linux ./focaswebapi [port]"
