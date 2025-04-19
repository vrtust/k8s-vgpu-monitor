# k8s-vgpu-monitor

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](./LICENSE)
[![C++ Standard](https://img.shields.io/badge/C++-11-blue.svg)]()

**[简体中文](./readme_zh.md)** | English

A monitoring tool developed using C++ and NVML to accurately track the **vGPU memory usage** and **SM (Streaming Multiprocessor) utilization** for each Pod in a Kubernetes (K8s) environment, based on a configured **GPU virtualization ratio**. It exposes monitoring metrics via a Prometheus Exporter interface.

---

## Table of Contents

- [Project Background and Motivation](#project-background-and-motivation)
- [Core Features](#core-features)
- [How It Works](#how-it-works)
- [System Requirements](#system-requirements)
- [Dependencies](#dependencies)
- [Installation and Build](#installation-and-build)
- [Deployment and Usage](#deployment-and-usage)
- [Configuration](#configuration)
- [Exposed Prometheus Metrics](#exposed-prometheus-metrics)
- [Development and Contribution](#development-and-contribution)
- [License](#license)

---

## Project Background and Motivation

When using GPUs in a Kubernetes cluster, especially when employing **GPU virtualization techniques** (e.g., implemented via open-source tools allowing multiple Pods to share a physical GPU), accurately monitoring the actual GPU resources (like memory, compute unit utilization) consumed by each Pod becomes crucial. Standard Kubernetes monitoring solutions often lack visibility into this fine-grained vGPU resource usage.

This project aims to address this pain point by providing a lightweight, efficient monitoring agent that can:

1.  Identify every process running on the physical GPU.
2.  Associate processes with their respective Kubernetes Pods.
3.  Aggregate and calculate the vGPU memory usage and SM utilization for each Pod based on the configured **GPU virtualization ratio**.
4.  Expose these metrics via a standard Prometheus interface for integration into existing monitoring and alerting systems.

## Core Features

*   Monitors physical GPU memory usage and SM utilization per process using the NVML API.
*   Identifies the Kubernetes Pod and container a process belongs to by parsing the `/proc/[pid]/cgroup` file.
*   Calculates Pod vGPU metrics based on the GPU virtualization ratio specified in the `gpu_allocation.txt` configuration file.
*   Aggregates GPU resource usage data for all processes within the same Pod.

## How It Works

1.  **Initialization:**
    *   Reads the `gpu_allocation.txt` file at startup to get the global GPU virtualization ratio.
    *   Initializes the NVML library to get information about the physical GPUs on the node.
    *   Initializes the `prometheus-cpp` Exporter to listen on the fixed port `8080`.
    *   Initializes the `spdlog` logging system.
2.  **Periodic Monitoring (Hardcoded Interval):**
    *   Periodically performs the following actions:
    *   Iterates through all physical GPU devices on the node.
    *   For each GPU, uses NVML to get the list of currently running processes, their PIDs, physical GPU memory usage (`usedGpuMemory`), and SM utilization (`smUtil`).
    *   For each process PID:
        *   Reads the `/proc/[pid]/cgroup` file.
        *   Parses the cgroup information to extract the Kubernetes Pod UID or container ID, thereby determining which Pod the process belongs to.
    *   **Aggregation and Calculation:**
        *   Aggregates the physical GPU memory usage and SM utilization for all processes belonging to the same Pod.
        *   **vGPU Metric Conversion (Based on Configured Ratio):**
            *   `pod_gpu_memory_used`: Aggregated physical GPU memory usage of the Pod.
            *   `pod_gpu_sm_util`: Aggregated physical SM utilization of the Pod.
            *   `pod_total_gpu_memory`: Total vGPU memory allocated to the Pod = `Total Physical GPU Memory / Virtualization Ratio`.
3.  **Metric Exposure:**
    *   Updates the calculated Pod-level metrics into the `prometheus-cpp` Gauges.
    *   A Prometheus server can scrape these metrics by accessing `http://<node-ip>:8080/metrics`.

## System Requirements

*   **Operating System:** Linux (requires access to `/proc` filesystem and cgroup information)
*   **Hardware:** At least one NVIDIA GPU supporting NVML
*   **Driver:** NVIDIA driver installed, including the NVML library (`libnvidia-ml.so`)
*   **Permissions:** Sufficient permissions to access `/proc/[pid]/cgroup` (usually root or specific capabilities) and interact with the NVML library.
*   **Compiler:** C++11 compliant C++ compiler (e.g., g++)
*   **Build Tool:** CMake (>= 3.10)
*   **Configuration File:** A file named `gpu_allocation.txt` must exist in the program's running directory.

## Dependencies

*   **NVML (NVIDIA Management Library):** Typically installed with the NVIDIA driver. Development requires header files (possibly in `libnvidia-ml-dev` or a similar package).
*   **prometheus-cpp:** [https://github.com/jupp0r/prometheus-cpp](https://github.com/jupp0r/prometheus-cpp)
    *   **Integration Method:** **As a prerequisite**. `prometheus-cpp` needs to be installed on the system beforehand (e.g., via package manager or manual compilation and installation).
*   **simdjson:** [https://github.com/simdjson/simdjson](https://github.com/simdjson/simdjson)
    *   **Integration Method:** Automatically downloaded and integrated via CMake `FetchContent`.
*   **spdlog:** [https://github.com/gabime/spdlog](https://github.com/gabime/spdlog)
    *   **Integration Method:** Automatically downloaded and integrated via CMake `FetchContent`.

## Installation and Build

**1. Install System Dependencies**

First, ensure that the necessary compilation tools and libraries are installed. The following command is suitable for Debian/Ubuntu-based systems:

```bash
apt update -y
apt install -y g++ cmake libcurl4-openssl-dev
```

**2. Install prometheus-cpp (As a Prerequisite)**

This project depends on the prometheus-cpp library. Compile and install it from source first:

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

**3. Build This Project (k8s-vgpu-monitor)**

Now you can compile k8s-vgpu-monitor. The project provides a `build.sh` script to simplify this process:

```bash
# 1. Clone the repository
git clone [Your Project Repository URL]
cd k8s-vgpu-monitor

# 2. Run the build script
bash build.sh
```

The `build.sh` script performs the following actions:

1.  Creates a `build` directory and enters it.
2.  Runs `cmake ..` to configure the project. CMake will download `simdjson` and `spdlog` via FetchContent and find the already installed `prometheus-cpp` and `NVML`.
3.  Runs `cmake ..` again (possibly to ensure dependencies are fully configured) and then runs `cmake --build .` (equivalent to `make`) to compile.
4.  **Note:** The last line of the script `echo "2" > gpu_allocation.txt` creates an example configuration file in the `build` directory. You need to modify it according to your actual GPU virtualization ratio and ensure the program can find this file at runtime (place it next to the executable).

After successful compilation, the executable `vgpu_monitor` will be located in the `build` directory.

**4. Build Docker Image**

```bash
docker build -t vgpu_monitor .
```

## Deployment and Usage

1.  **Prepare Configuration File:** In the directory where `vgpu_monitor` will run, create a file named `gpu_allocation.txt`. The content of this file should be **only a single number**, representing the GPU virtualization ratio. For example, if one physical GPU is virtualized into 2 vGPUs for Pods, the file content should be `2`.
2.  **Kubernetes Deployment:** Use the `gpu-monitor.yml` file provided in the project to deploy this monitoring program as a DaemonSet to all nodes in the cluster that have GPUs.

    Please modify `/path/to/gpu_allocation.txt` to the actual path.

    ```yaml
        - name: hostconf
          hostPath:
            path: /path/to/gpu_allocation.txt # Modify this path
    ```

    Run the following command to deploy:

    ```bash
    kubectl apply -f kubernetes/vgpu_monitor.yml
    ```

3.  **Accessing Metrics:** After the program starts, you can access the Prometheus metrics endpoint via `http://<node-ip-where-program-runs>:8080/metrics`.

## Configuration

Configuration options for this project are currently very limited:

*   **GPU Virtualization Ratio:**
    *   **Configuration Method:** By creating the `gpu_allocation.txt` file.
    *   **Content:** The file contains only a single integer or floating-point number representing the virtualization ratio. The program reads this file on startup.
    *   **Example:** File content `2` means 1 physical card is virtualized into 2 vGPUs.

*   **Hardcoded Configuration (Not Externally Modifiable):**
    *   **Prometheus Metrics Port:** Fixed at `8080`.
    *   **Monitoring Poll Interval:** Fixed value embedded in the code.

## Exposed Prometheus Metrics

The program exposes the following Pod-level GPU metrics:

```
# HELP pod_gpu_sm_util GPU SM utilization per pod (Aggregated physical utilization)
# TYPE pod_gpu_sm_util gauge
pod_gpu_sm_util{gpu_id="[GPU_INDEX]",pod="[POD_NAME]"} [VALUE]

# HELP pod_gpu_memory_used GPU memory usage per pod (Aggregated physical usage, in MB)
# TYPE pod_gpu_memory_used gauge
pod_gpu_memory_used{gpu_id="[GPU_INDEX]",pod="[POD_NAME]"} [VALUE]

# HELP pod_total_gpu_memory Total GPU memory per pod (Allocated vGPU memory based on ratio, in MB)
# TYPE pod_total_gpu_memory gauge
pod_total_gpu_memory{gpu_id="[GPU_INDEX]",pod="[POD_NAME]"} [VALUE]

# --- Standard Exporter Metrics from prometheus-cpp library (Example) ---
# HELP exposer_transferred_bytes_total Transferred bytes to metrics services
# TYPE exposer_transferred_bytes_total counter
# HELP exposer_request_latencies Latencies of serving metrics requests.
# TYPE exposer_request_latencies summary
# ... other metrics possibly exposed by prometheus-cpp
```

**Metric Descriptions:**

*   `pod_gpu_sm_util`: (Gauge) Aggregated **physical SM utilization(%)** for a single Pod on the specified GPU.
*   `pod_gpu_memory_used`: (Gauge) Aggregated **physical GPU memory usage(MB)** for a single Pod on the specified GPU.
*   `pod_total_gpu_memory`: (Gauge) Calculated **total vGPU memory(MB)** allocated to the Pod on the specified GPU, based on the ratio in `gpu_allocation.txt`.

**Labels:**

*   `gpu_id`: Index or ID of the GPU (starting from 0). Corresponds to the device index returned by NVML. (Note: Text uses `gpu_id` but example shows `gpu_id`. This reflects the original source.)
*   `pod`: Detected Kubernetes Pod name.

## Development and Contribution

Contributions to this project are welcome! Here are some potential areas for improvement:

*   **Increase Configurability:**
    *   Make the Prometheus listening port (`8080`) configurable via command-line arguments or environment variables.
    *   Make the monitoring poll interval configurable.
    *   Allow specifying the path to `gpu_allocation.txt` via a command-line argument.
*   **Enhance Metrics:**
    *   Add a `namespace` label to all Pod metrics for global uniqueness.
*   **Testing:** Add unit tests and integration tests.
*   **Documentation:** Improve code comments and documentation details.

**Contribution Workflow:**

1.  Fork this repository.
2.  Create a new feature branch (`git checkout -b feature/YourFeature`).
3.  Make your changes and develop the feature.
4.  Ensure code style consistency.
5.  Commit your changes (`git commit -am 'Add some feature'`).
6.  Push to the branch (`git push origin feature/YourFeature`).
7.  Create a Pull Request.

## License

This project is licensed under the **MIT License**. See the [LICENSE](./LICENSE) file for details.
