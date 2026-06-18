#!/bin/bash

rm -rf build
mkdir build
cd build

export PATH=/usr/local/cuda/bin:$PATH

cmake .. -DGGML_CPU_ALL_VARIANTS=ON -DGGML_CUDA=ON -DGGML_VULKAN=ON -DGGML_BACKEND_DL=ON -DTOOLS=ON -DMP3=ON -DOPUS=ON -DFLAC=ON
cmake --build . --config Release -j "$(nproc)"
