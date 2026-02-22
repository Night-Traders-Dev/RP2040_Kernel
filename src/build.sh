#!/usr/bin/env bash

set -e

export PICO_SDK_PATH=/opt/pico-sdk

mkdir -p build
cd build

cmake .. -DPICO_BOARD=pico
make -j$(nproc)
