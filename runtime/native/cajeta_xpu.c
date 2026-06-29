// === cajeta_xpu.c — XPU/GPU module aggregator (one tightly-coupled module:
// === driver, dispatch, textures, Vulkan, accel, launch share g_xpu_* state).
// === #included into cajeta_runtime.c (single-TU build).

#include "cajeta_xpu_driver.c"
#include "cajeta_xpu_vulkan.c"
#include "cajeta_xpu_dispatch.c"
#include "cajeta_xpu_texture_amd.c"
#include "cajeta_xpu_texture_cuda.c"
#include "cajeta_xpu_accel.c"
#include "cajeta_xpu_launch.c"
