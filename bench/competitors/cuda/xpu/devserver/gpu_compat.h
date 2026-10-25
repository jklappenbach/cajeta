// gpu_compat.h — one source, two vendors: devserver.cu builds with nvcc (CUDA,
// CUB, cuBLAS) or hipcc (HIP, hipCUB over rocPRIM, hipBLAS over rocBLAS).
// competitors/hip/xpu/devserver/ builds this same file with hipcc on Proton.
#pragma once
#if defined(__HIPCC__)
#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include <hip/hip_bf16.h>
#include <hipcub/hipcub.hpp>
#include <hipblas/hipblas.h>
#define GPU_NAME "HIP"
#define GPU_BACKEND "hip"
namespace gpucub = hipcub;
typedef hipError_t gpuError_t;
typedef hipStream_t gpuStream_t;
typedef hipEvent_t gpuEvent_t;
typedef hipArray_t gpuArray_t;
typedef hipTextureObject_t gpuTextureObject_t;
typedef hipDeviceProp_t gpuDeviceProp_t;
typedef hipChannelFormatDesc gpuChannelFormatDesc;
typedef hipResourceDesc gpuResourceDesc;
typedef hipTextureDesc gpuTextureDesc;
typedef hipblasHandle_t gpublasHandle_t;
typedef hipblasStatus_t gpublasStatus_t;
typedef __half gpu_half;
typedef hip_bfloat16 gpu_bf16;
#define gpuSuccess hipSuccess
#define gpuGetErrorString hipGetErrorString
#define gpuRuntimeGetVersion hipRuntimeGetVersion
#define gpuMalloc hipMalloc
#define gpuFree hipFree
#define gpuMemcpy hipMemcpy
#define gpuMemcpyHostToDevice hipMemcpyHostToDevice
#define gpuMemcpyDeviceToHost hipMemcpyDeviceToHost
#define gpuDeviceSynchronize hipDeviceSynchronize
#define gpuGetDeviceProperties hipGetDeviceProperties
#define gpuGetLastError hipGetLastError
#define gpuStreamCreateWithFlags hipStreamCreateWithFlags
#define gpuStreamNonBlocking hipStreamNonBlocking
#define gpuStreamSynchronize hipStreamSynchronize
#define gpuStreamDestroy hipStreamDestroy
#define gpuEventCreate hipEventCreate
#define gpuEventRecord hipEventRecord
#define gpuEventSynchronize hipEventSynchronize
#define gpuEventElapsedTime hipEventElapsedTime
#define gpuEventDestroy hipEventDestroy
#define gpuCreateChannelDesc hipCreateChannelDesc
#define gpuMallocArray hipMallocArray
#define gpuMemcpy2DToArray hipMemcpy2DToArray
#define gpuFreeArray hipFreeArray
#define gpuResourceTypeArray hipResourceTypeArray
#define gpuAddressModeClamp hipAddressModeClamp
#define gpuFilterModeLinear hipFilterModeLinear
#define gpuReadModeNormalizedFloat hipReadModeNormalizedFloat
#define gpuCreateTextureObject hipCreateTextureObject
#define gpuDestroyTextureObject hipDestroyTextureObject
#define gpu_float2half __float2half
#define gpu_float2bf16(f) hip_bfloat16(f)
#define GPUBLAS_STATUS_SUCCESS HIPBLAS_STATUS_SUCCESS
#define GPUBLAS_OP_N HIPBLAS_OP_N
#define gpublasCreate hipblasCreate
#define gpublasDestroy hipblasDestroy
#define gpublasDgemm hipblasDgemm
#define gpublasSgemm hipblasSgemm
#define gpublasGemmEx hipblasGemmEx_v2
#define GPU_R_16F HIP_R_16F
#define GPU_R_16BF HIP_R_16BF
#define GPU_R_32F HIP_R_32F
#define GPUBLAS_COMPUTE_32F HIPBLAS_COMPUTE_32F
#define GPUBLAS_GEMM_DEFAULT HIPBLAS_GEMM_DEFAULT
#else
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <cuda_bf16.h>
#include <cub/cub.cuh>
#include <cublas_v2.h>
#define GPU_NAME "CUDA"
#define GPU_BACKEND "cuda"
namespace gpucub = cub;
typedef cudaError_t gpuError_t;
typedef cudaStream_t gpuStream_t;
typedef cudaEvent_t gpuEvent_t;
typedef cudaArray_t gpuArray_t;
typedef cudaTextureObject_t gpuTextureObject_t;
typedef cudaDeviceProp gpuDeviceProp_t;
typedef cudaChannelFormatDesc gpuChannelFormatDesc;
typedef cudaResourceDesc gpuResourceDesc;
typedef cudaTextureDesc gpuTextureDesc;
typedef cublasHandle_t gpublasHandle_t;
typedef cublasStatus_t gpublasStatus_t;
typedef __half gpu_half;
typedef __nv_bfloat16 gpu_bf16;
#define gpuSuccess cudaSuccess
#define gpuGetErrorString cudaGetErrorString
#define gpuRuntimeGetVersion cudaRuntimeGetVersion
#define gpuMalloc cudaMalloc
#define gpuFree cudaFree
#define gpuMemcpy cudaMemcpy
#define gpuMemcpyHostToDevice cudaMemcpyHostToDevice
#define gpuMemcpyDeviceToHost cudaMemcpyDeviceToHost
#define gpuDeviceSynchronize cudaDeviceSynchronize
#define gpuGetDeviceProperties cudaGetDeviceProperties
#define gpuGetLastError cudaGetLastError
#define gpuStreamCreateWithFlags cudaStreamCreateWithFlags
#define gpuStreamNonBlocking cudaStreamNonBlocking
#define gpuStreamSynchronize cudaStreamSynchronize
#define gpuStreamDestroy cudaStreamDestroy
#define gpuEventCreate cudaEventCreate
#define gpuEventRecord cudaEventRecord
#define gpuEventSynchronize cudaEventSynchronize
#define gpuEventElapsedTime cudaEventElapsedTime
#define gpuEventDestroy cudaEventDestroy
#define gpuCreateChannelDesc cudaCreateChannelDesc
#define gpuMallocArray cudaMallocArray
#define gpuMemcpy2DToArray cudaMemcpy2DToArray
#define gpuFreeArray cudaFreeArray
#define gpuResourceTypeArray cudaResourceTypeArray
#define gpuAddressModeClamp cudaAddressModeClamp
#define gpuFilterModeLinear cudaFilterModeLinear
#define gpuReadModeNormalizedFloat cudaReadModeNormalizedFloat
#define gpuCreateTextureObject cudaCreateTextureObject
#define gpuDestroyTextureObject cudaDestroyTextureObject
#define gpu_float2half __float2half
#define gpu_float2bf16 __float2bfloat16
#define GPUBLAS_STATUS_SUCCESS CUBLAS_STATUS_SUCCESS
#define GPUBLAS_OP_N CUBLAS_OP_N
#define gpublasCreate cublasCreate
#define gpublasDestroy cublasDestroy
#define gpublasDgemm cublasDgemm
#define gpublasSgemm cublasSgemm
#define gpublasGemmEx cublasGemmEx
#define GPU_R_16F CUDA_R_16F
#define GPU_R_16BF CUDA_R_16BF
#define GPU_R_32F CUDA_R_32F
#define GPUBLAS_COMPUTE_32F CUBLAS_COMPUTE_32F
#define GPUBLAS_GEMM_DEFAULT CUBLAS_GEMM_DEFAULT
#endif
