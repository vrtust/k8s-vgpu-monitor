cmake_minimum_required(VERSION 3.10)
project(vgpu_monitor LANGUAGES CUDA CXX)

# 设置 CUDA 编译器
enable_language(CUDA)
set(CUDA_NVCC_FLAGS "-std=c++11 -g")

include(FetchContent)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_SHALLOW TRUE
    GIT_TAG        v1.x
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG  tags/v3.6.0
  GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(simdjson)

# 添加可执行文件
add_executable(vgpu_monitor vgpu_monitor.cpp)

find_package(CUDAToolkit REQUIRED)

# 链接库
target_link_libraries(vgpu_monitor
    PRIVATE
        spdlog
        CUDA::nvml
        prometheus-cpp-pull
        prometheus-cpp-core
        z
        curl
        simdjson)