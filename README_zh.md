# k8s-vgpu-monitor

[![许可证](https://img.shields.io/badge/license-MIT-blue.svg)](./LICENSE)
[![C++ Standard](https://img.shields.io/badge/C++-11-blue.svg)]()

**简体中文** | **[English](./README.md)**

一个使用 C++ 和 NVML 开发的监控工具，用于精确统计 Kubernetes (K8s) 环境中每个 Pod 基于 **GPU 虚拟化比例** 的 **vGPU 显存占用** 和 **SM (Streaming Multiprocessor) 利用率**，并通过 Prometheus Exporter 接口暴露监控指标。

---

## 目录

- [项目背景与动机](#项目背景与动机)
- [核心功能](#核心功能)
- [工作原理](#工作原理)
- [系统需求](#系统需求)
- [依赖项](#依赖项)
- [安装与构建](#安装与构建)
- [部署与使用](#部署与使用)
- [配置](#配置)
- [暴露的 Prometheus 指标](#暴露的-prometheus-指标)
- [开发与贡献](#开发与贡献)
- [许可证](#许可证)

---

## 项目背景与动机

在 Kubernetes 集群中使用 GPU 时，尤其是当采用 **GPU 虚拟化技术**（例如通过一些开源工具实现，允许多个 Pod 共享物理 GPU）时，精确监控每个 Pod 实际消耗的 GPU 资源（如显存、计算单元利用率）变得非常重要。标准的 Kubernetes 监控方案往往缺乏对这种细粒度 vGPU 资源使用的可见性。

本项目旨在解决这一痛点，提供一个轻量级、高效的监控代理，它能够：

1.  识别出运行在物理 GPU 上的每个进程。
2.  将进程与其所属的 Kubernetes Pod 相关联。
3.  根据配置的 **GPU 虚拟化比例**，聚合计算出每个 Pod 的 vGPU 显存使用量和 SM 利用率。
4.  通过标准的 Prometheus 接口暴露这些指标，以便集成到现有的监控告警体系中。

## 核心功能

*   使用 NVML API 监控每个进程的物理 GPU 显存使用量和 SM 利用率。
*   通过解析 `/proc/[pid]/cgroup` 文件，识别进程所属的 Kubernetes Pod 和容器。
*   根据配置文件 `gpu_allocation.txt` 中指定的 GPU 虚拟化比例，计算 Pod 的 vGPU 指标。
*   聚合同一 Pod 内所有进程的 GPU 资源使用数据。

## 工作原理

1.  **初始化:**
    *   程序启动时读取 `gpu_allocation.txt` 文件，获取全局 GPU 虚拟化比例。
    *   初始化 NVML 库，获取节点上的物理 GPU 信息。
    *   初始化 `prometheus-cpp` Exporter，监听固定端口 `8080`。
    *   初始化 `spdlog` 日志系统。
2.  **周期性监控 (硬编码间隔):**
    *   定期执行以下操作：
    *   遍历节点上的所有物理 GPU 设备。
    *   对于每个 GPU，使用 NVML 获取当前运行的所有进程列表及其 PID、物理显存占用 (`usedGpuMemory`)、SM 利用率 (`smUtil`)。
    *   对于每个进程 PID：
        *   读取 `/proc/[pid]/cgroup` 文件。
        *   解析 cgroup 信息，提取 Kubernetes Pod UID 或容器 ID，从而确定该进程属于哪个 Pod。
    *   **聚合计算:**
        *   按 Pod 聚合所有属于该 Pod 的进程的物理显存占用和 SM 利用率。
        *   **vGPU 指标转换 (基于配置的比例):**
            *   `pod_gpu_memory_used`: 聚合后的 Pod 物理显存占用。
            *   `pod_gpu_sm_util`: 聚合后的 Pod 物理 SM 利用率。
            *   `pod_total_gpu_memory`: Pod 分配到的 vGPU 总显存 = `物理 GPU 总显存 / 虚拟化比例`。
3.  **指标暴露:**
    *   将计算得到的 Pod 级别指标更新到 `prometheus-cpp` 的 Gauges 中。
    *   Prometheus 服务器可以访问 `http://<node-ip>:8080/metrics` 来抓取这些指标。

## 系统需求

*   **操作系统:** Linux (需要访问 `/proc` 文件系统和 cgroup 信息)
*   **硬件:** 至少一张支持 NVML 的 NVIDIA GPU
*   **驱动:** 已安装 NVIDIA 驱动程序，并包含 NVML 库 (`libnvidia-ml.so`)
*   **权限:** 需要足够权限访问 `/proc/[pid]/cgroup` (通常为 root 或特定 capability) 和与 NVML 库交互。
*   **编译器:** 支持 C++11 的 C++ 编译器 (如 g++)
*   **构建工具:** CMake (>= 3.10)
*   **配置文件:** 必须在程序运行目录下存在 `gpu_allocation.txt` 文件。

## 依赖项

*   **NVML (NVIDIA Management Library):** 通常随 NVIDIA 驱动安装。开发需要头文件 (可能在 `libnvidia-ml-dev` 或类似包中)。
*   **prometheus-cpp:** [https://github.com/jupp0r/prometheus-cpp](https://github.com/jupp0r/prometheus-cpp)
    *   **集成方式:** **作为先决条件**。需要预先在系统中安装好 `prometheus-cpp` (例如通过包管理器、或手动编译安装)。
*   **simdjson:** [https://github.com/simdjson/simdjson](https://github.com/simdjson/simdjson)
    *   **集成方式:** 通过 CMake `FetchContent` 自动下载和集成。
*   **spdlog:** [https://github.com/gabime/spdlog](https://github.com/gabime/spdlog)
    *   **集成方式:** 通过 CMake `FetchContent` 自动下载和集成。

## 安装与构建

**1. 安装系统依赖**

首先，确保系统已安装必要的编译工具和库。以下命令适用于基于 Debian/Ubuntu 的系统：

```bash
apt update -y
apt install -y g++ cmake libcurl4-openssl-dev
```

**2. 安装 prometheus-cpp （作为先决条件）**

本项目依赖 prometheus-cpp 库。从源码编译并安装它：

```bash
git clone https://github.com/jupp0r/prometheus-cpp.git
cd prometheus-cpp
mkdir _build
cd _build
cmake .. -DBUILD_SHARED_LIBS=ON -DENABLE_PUSH=OFF -DENABLE_COMPRESSION=OFF
cmake --build . --parallel 4
ctest -V
cmake --install . && \
ldconfig
```

**3. 构建本项目（k8s-vgpu-monitor）**

现在可以编译 k8s-vgpu-monitor 了。项目提供了 build.sh 脚本来简化此过程：

```bash
# 1. 克隆仓库
git clone https://github.com/vrtust/k8s-vgpu-monitor.git
cd k8s-vgpu-monitor

# 2. 运行构建脚本
bash build.sh
```

`build.sh` 脚本执行以下操作：

1.  创建 `build` 目录并进入。
2.  运行 `cmake ..` 配置项目，CMake 会通过 FetchContent 下载 `simdjson` 和 `spdlog`，并查找已安装的 `prometheus-cpp` 和 `NVML`。
3.  再次运行 `cmake ..` (可能是为了确保依赖项完全配置) 并运行 `cmake --build .` (等同于 `make`) 进行编译。
4.  **注意:** 脚本最后一行 `echo "2" > gpu_allocation.txt` 会在 `build` 目录下创建一个示例配置文件，需要根据实际的 GPU 虚拟化比例修改它，并确保程序运行时能找到这个文件（放在可执行文件旁边）。

编译成功后，可执行文件 `vgpu_monitor` 会出现在 `build` 目录下。

**4. 构建镜像**

```bash
docker build -t vgpu_monitor .
```

## 部署与使用

1.  **准备配置文件:** 在运行 `vgpu_monitor` 的路径下，创建一个名为 `gpu_allocation.txt` 的文件。文件内容**仅包含一个数字**，代表 GPU 的虚拟化比例。例如，如果一台物理 GPU 被虚拟化成 2 个 vGPU 给 Pod 使用，文件内容就是 `2`。
2.  **Kubernetes 部署:** 使用项目提供的 `gpu-monitor.yml` 文件将此监控程序部署到集群中所有包含 GPU 的节点上。

    请修改 `/path/to/gpu_allocation.txt` 为实际径。

    ```yml
        - name: hostconf
          hostPath: 
            path: /path/to/gpu_allocation.txt # 修改该路径
    ```

    运行以下命令进行部署：

    ```bash
    kubectl apply -f kubernetes/vgpu_monitor.yml
    ```

3.  **访问指标:** 程序启动后，可以通过 `http://<运行程序的节点IP>:8080/metrics` 访问 Prometheus 指标端点。

## 配置

本项目目前配置项非常有限：

*   **GPU 虚拟化比例:**
    *   **配置方式:** 通过创建 `gpu_allocation.txt` 文件。
    *   **内容:** 文件内只包含一个整数或浮点数，代表虚拟化比例。程序启动时读取此文件。
    *   **示例:** 文件内容为 `2` 表示 1 张物理卡虚拟为 2 个 vGPU。

*   **硬编码配置 (不可外部修改):**
    *   **Prometheus 指标端口:** 固定为 `8080`。
    *   **监控轮询间隔:** 固定值，嵌入在代码中。

## 暴露的 Prometheus 指标

程序暴露以下 Pod 级别的 GPU 指标：

```
# HELP pod_gpu_sm_util GPU SM utilization per pod (Aggregated physical utilization)
# TYPE pod_gpu_sm_util gauge
pod_gpu_sm_util{gpu_id="[GPU_INDEX]",pod="[POD_NAME]"} [VALUE]

# HELP pod_gpu_memory_used GPU memory usage per pod (Aggregated physical usage, likely in Bytes)
# TYPE pod_gpu_memory_used gauge
pod_gpu_memory_used{gpu_id="[GPU_INDEX]",pod="[POD_NAME]"} [VALUE]

# HELP pod_total_gpu_memory Total GPU memory per pod (Allocated vGPU memory based on ratio, likely in Bytes)
# TYPE pod_total_gpu_memory gauge
pod_total_gpu_memory{gpu_id="[GPU_INDEX]",pod="[POD_NAME]"} [VALUE]

# --- 来自 prometheus-cpp 库的标准 Exporter 指标 (示例) ---
# HELP exposer_transferred_bytes_total Transferred bytes to metrics services
# TYPE exposer_transferred_bytes_total counter
# HELP exposer_request_latencies Latencies of serving metrics requests.
# TYPE exposer_request_latencies summary
# ... 其他 prometheus-cpp 可能暴露的指标
```

**指标说明:**

*   `pod_gpu_sm_util`: (Gauge) 单个 Pod 在指定 GPU 上聚合的**物理 SM 利用率(%)**。
*   `pod_gpu_memory_used`: (Gauge) 单个 Pod 在指定 GPU 上聚合的**物理显存使用量(MB)**。
*   `pod_total_gpu_memory`: (Gauge) 根据 `gpu_allocation.txt` 配置的比例，计算出的该 Pod 在指定 GPU 上分配到的 **vGPU 总显存(MB)**。

**标签 (Labels):**

*   `gpu_id`: GPU 的索引号或 ID (从 0 开始)。对应于 NVML 返回的设备索引。
*   `pod`: 检测到的 Kubernetes Pod 名称。

## 开发与贡献

欢迎对本项目进行贡献！以下是一些可能的改进方向：

*   **增加配置项:**
    *   将 Prometheus 监听端口 (`8080`) 改为可通过命令行参数或环境变量配置。
    *   将监控轮询间隔改为可配置。
    *   允许通过命令行参数指定 `gpu_allocation.txt` 的路径。
*   **增强指标:**
    *   在所有 Pod 指标中添加 `namespace` 标签，以确保全局唯一性。
*   **测试:** 添加单元测试和集成测试。
*   **文档:** 完善代码注释和文档细节。

**贡献流程:**

1.  Fork 本仓库。
2.  创建新的特性分支 (`git checkout -b feature/YourFeature`)。
3.  进行修改和开发。
4.  确保代码风格一致。
5.  提交修改 (`git commit -am 'Add some feature'`)。
6.  推送 H 到分支 (`git push origin feature/YourFeature`)。
7.  创建 Pull Request。

## 许可证

本项目采用 **MIT 许可证** 授权。详情请参阅 [LICENSE](./LICENSE) 文件。