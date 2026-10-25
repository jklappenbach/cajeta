#!/usr/bin/env bash
# gfx/ device competitors: the devserver's texture-sample-rate workload (CUDA
# texture objects; HIP on Proton). The build and the rules are drivers/xpu/device.sh's.
# Vulkan sampled images (the page's second competitor) have no program yet.
XPU_DEVICE_AREA=gfx exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../xpu" &> /dev/null && pwd )/device.sh"
