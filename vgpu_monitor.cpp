#include <iostream>
#include <thread> // For sleep
#include <nvml.h>
#include <vector>
#include <sstream>
#include <string>
#include <chrono>
#include <fstream>
#include <map>
#include <regex>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/gauge.h>
#include <set>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <curl/curl.h>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "simdjson.h"
using namespace simdjson;

std::vector<std::string> gpu_ids;
std::vector<std::string> gpu_uuids;
std::map<std::string, std::map<int, int>> gpu_usage;
std::map<std::string, std::string> pod_id_to_docker_id;
unsigned long long gpu_memory = 0;
std::map<std::string, std::map<std::string, unsigned int>> gpu_index;
std::map<std::string, std::string> pod_uid_to_id;
int GPUAllocation = 0;

// Function to initialize the logger
void init_logger()
{
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_st>();
    auto logger = std::make_shared<spdlog::logger>("multi_sink", spdlog::sinks_init_list{ console_sink });
    logger->set_level(spdlog::level::debug);
    // [%Y-%m-%d %H:%M:%S.%e] Time
    // [%l] Log level
    // [%v] Actual text
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %^[%=7l]%$ %v");
    spdlog::set_default_logger(logger);
}

void get_gpu_uuids()
{
    unsigned int deviceCount;
    nvmlReturn_t result;

    // Get the number of devices
    result = nvmlDeviceGetCount(&deviceCount);
    if (NVML_SUCCESS != result) {
        throw std::runtime_error("Failed to get device count: " +
            std::string(nvmlErrorString(result)));
    }

    // Iterate through all devices
    for (unsigned int i = 0; i < deviceCount; i++) {
        nvmlDevice_t device;
        char uuid[NVML_DEVICE_UUID_BUFFER_SIZE];

        // Get the device handle
        result = nvmlDeviceGetHandleByIndex(i, &device);
        if (NVML_SUCCESS != result) {
                spdlog::error("Failed to get device handle for device {}: {}", i, nvmlErrorString(result));
            continue;
        }

        // Get the device UUID
        result = nvmlDeviceGetUUID(device, uuid, NVML_DEVICE_UUID_BUFFER_SIZE);
        if (NVML_SUCCESS != result) {
            spdlog::error("Failed to get UUID for device {}: {}", i, nvmlErrorString(result));
            continue;
        }

        gpu_uuids.push_back(std::string(uuid));
    }
}

// Parse GPU UUIDs from Docker environment variables
std::vector<std::string> parse_docker_gpu_uuids(const std::string& envValue)
{
    std::vector<std::string> dockerUUIDs;
    std::stringstream ss(envValue);
    std::string uuid;

    while (std::getline(ss, uuid, ',')) {
        // std::cout << "Found GPU UUID: " << uuid << std::endl;
        // Remove possible spaces
        uuid.erase(0, uuid.find_first_not_of(" "));
        uuid.erase(uuid.find_last_not_of(" ") + 1);
        dockerUUIDs.push_back(uuid);
    }
    return dockerUUIDs;
}

// Count GPU usage
std::map<int, int> count_gpu_usage(const std::string& dockerGPUs)
{
    std::map<int, int> gpuUsageCount; // <GPU index, count>

    // Parse GPU UUIDs from Docker environment variables
    std::vector<std::string> dockerUUIDs = parse_docker_gpu_uuids(dockerGPUs);

    // Count the occurrences of each GPU index
    for (const auto& dockerUUID : dockerUUIDs) {
        for (size_t i = 0; i < gpu_uuids.size(); i++) {
            if (gpu_uuids[i].find(dockerUUID) != std::string::npos) {
                gpuUsageCount[i]++;
            }
        }
    }

    return gpuUsageCount;
}

std::string execute_get_docker_gpus_command(const std::string& command)
{
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);

    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

void get_docker_gpus(const std::string& containerId)
{
    if (gpu_usage.find(containerId) != gpu_usage.end()) {
        return;
    }
    std::string command = "docker inspect " + containerId + " | grep NVIDIA_VISIBLE_DEVICES";
    std::string output = execute_get_docker_gpus_command(command);

    // Extract GPU UUID
    size_t start = output.find("=") + 1;
    size_t end = output.find("\",");
    if (start == std::string::npos || end == std::string::npos) {
        throw std::runtime_error("Failed to parse docker inspect output");
    }

    auto originalGPUUsage = count_gpu_usage(output.substr(start, end - start));
    std::map<int, int> adjustedGPUUsage;

    // Extract original keys (already sorted automatically)
    std::vector<int> sortedKeys;
    for (const auto& pair : originalGPUUsage) {
        sortedKeys.push_back(pair.first);
    }

    // Build a new map with consecutive index keys
    for (size_t i = 0; i < sortedKeys.size(); ++i) {
        int originalKey = sortedKeys[i];
        adjustedGPUUsage[i] = originalGPUUsage[originalKey];
    }

    gpu_usage[containerId] = adjustedGPUUsage;
}

std::vector<unsigned long long> split_data(unsigned long long memory, int num_vgpu)
{
    std::vector<unsigned long long> split_values;
    for (int i = 0; i < num_vgpu; i++) {
        split_values.push_back(memory / num_vgpu);
    }
    return split_values;
}

int read_allocation()
{
    // File path
    const std::string filename = "gpu_allocation.txt";

    // Create an ifstream object to read the file
    std::ifstream infile(filename);

    // Check if the file was opened successfully
    if (!infile.is_open()) {
        spdlog::error("Unable to open file: {}", filename);
        return -1; // Return non-zero value to indicate failure
    }

    // Read the number from the file
    int gpu_allocation;
    infile >> gpu_allocation;

    // Check if the number was read successfully
    if (infile.fail()) {
        spdlog::error("Failed to read number from file: {}", filename);
        return -1;
    }

    // Output the read number
    spdlog::info("GPU Allocation: {}", gpu_allocation);

    GPUAllocation = gpu_allocation;

    // Close the file
    infile.close();

    return 0; // Return zero to indicate success
}

int get_gpus_in_host(const std::string& cmd, std::vector<std::string>& output)
{
    std::array<char, 128> buffer;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        spdlog::error("popen() failed!");
        return -1;
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        output.push_back(std::string(buffer.data()).substr(0, std::string(buffer.data()).length() - 1));
    }
    return 0;
}

int get_pod_id(const std::string& cmd, std::string& output)
{
    std::array<char, 128> buffer;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        spdlog::error("popen() failed!");
        return -1;
    }

    std::ostringstream oss;
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        oss << buffer.data();
    }
    output = oss.str().substr(0, oss.str().size() - 1); // Remove the last newline character
    return 0;
}

int get_gpu_id_in_pod(const std::string& cmd, std::string pod_id, std::map<std::string, std::map<unsigned int, std::pair<unsigned int, unsigned long long>>>& gpu_data)
{
    std::array<char, 128> buffer;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        spdlog::error("popen() failed!");
        return -1;
    }
    std::regex regex("/dev/nvidia([0-9]+)"); // Regex to match GPU ID
    std::smatch match;
    spdlog::info("cmd: {}", cmd);
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        std::string gpu_id = std::string(buffer.data()).substr(0, std::string(buffer.data()).length() - 1);
        if (!std::regex_search(gpu_id, match, regex)) {
            continue;
        }
        spdlog::info("gpu_id: {}", gpu_id);
        if (gpu_index[pod_id].find(gpu_id) == gpu_index[pod_id].end()) {
            spdlog::info("gpu_index[pod_id].size(): {}", gpu_index[pod_id].size());
            gpu_index[pod_id][gpu_id] = gpu_index[pod_id].size();
            gpu_data[pod_id][gpu_index[pod_id][gpu_id]] = std::make_pair(0, 0);
            spdlog::info("gpu_index[pod_id][gpu_id]: {}", gpu_index[pod_id][gpu_id]);
        }
    }
    return 0;
}

std::string extract_pod_id(const std::string& input_string)
{
    std::regex regex(R"(/pod([a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}))");
    std::smatch match;

    if (std::regex_search(input_string, match, regex)) {
        return match[1].str(); // Return the content of the first capture group
    }
    else {
        std::regex regex2(R"(-pod([a-f0-9]{8}_[a-f0-9]{4}_[a-f0-9]{4}_[a-f0-9]{4}_[a-f0-9]{12}))");
        std::smatch match2;
        if (std::regex_search(input_string, match2, regex2)) {
            return match2[1].str(); // Return the content of the first capture group
        }
        return ""; // No match found
    }
}

std::string extract_docker_id(const std::string& input_string)
{
    std::regex regex("/([a-f0-9]{64})"); // Match the 64-bit hexadecimal string after "/"
    std::smatch match;

    if (std::regex_search(input_string, match, regex)) {
        spdlog::info("match 1");
        return match[1].str(); // Return the content of the first capture group
    }
    else {
        std::regex regex2("docker-([a-f0-9]+)"); // Match the hexadecimal string after "docker-"
        std::smatch match2;
        if (std::regex_search(input_string, match2, regex2)) {
            spdlog::info("match 2");
            return match2[1].str(); // Return the content of the first capture group
        }
        spdlog::error("not match");
        return ""; // No match found
    }
}

int read_proc_cgroup(int pid, std::string& pod_uid, std::string& docker_id)
{
    char filename[256];
    char line[1024];

    FILE* fp;

    // Build the file path
    snprintf(filename, sizeof(filename), "/workspace/proc/%d/cgroup", pid);

    // Open the file
    fp = fopen(filename, "r");
    if (fp == NULL) {
        spdlog::error("can't open file");
        return -1;
    }

    fgets(line, sizeof(line), fp);
    std::string firstLine(line);
    spdlog::info("firstLine: {}", firstLine);

    docker_id = extract_docker_id(firstLine);
    if (docker_id == "") {
        spdlog::error("can't find docker id");
        fclose(fp);
        return -1;
    }

    pod_uid = extract_pod_id(firstLine);
    if (pod_uid == "") {
        spdlog::error("can't find pod id");
        fclose(fp);
        return -1;
    }
    std::replace(pod_uid.begin(), pod_uid.end(), '_', '-');

    fclose(fp);

    return 0;
}

void get_time(std::chrono::time_point<std::chrono::high_resolution_clock> start)
{
    // Record the end time
    auto end = std::chrono::high_resolution_clock::now();

    // Calculate the execution time
    std::chrono::duration<double> duration = end - start;

    // Output the execution time (in seconds)
    spdlog::info("Execution time: {} seconds", duration.count());
}

void get_usuage(std::map<std::string, std::map<unsigned int, std::pair<unsigned int, unsigned long long>>>& orginal_gpu_data, std::map<std::string, std::map<unsigned int, std::pair<unsigned int, unsigned long long>>>& gpu_data, nvmlDevice_t nvml_dev, unsigned int device_count, std::chrono::time_point<std::chrono::high_resolution_clock> start)
{
    nvmlReturn_t nvml_ret;

    std::map<int, unsigned long long> mem_record;

    if (gpu_ids.size() == 0) {
        std::string get_gpus_in_host_cmd = "find /dev -name 'nvidia[0-9]*' | sort -V";
        if (get_gpus_in_host(get_gpus_in_host_cmd, gpu_ids) != 0) {
            spdlog::error("exec {} failed", get_gpus_in_host_cmd);
            return;
        }
    }

    for (int i = 0; i < device_count; i++) {
        spdlog::info("");
        spdlog::info("current gpu is: {}", i);
        nvml_ret = nvmlDeviceGetHandleByIndex(i, &nvml_dev);
        int error_utilization = 0;
        std::map<std::string, unsigned int> total_gpu_util;
        std::map<std::string, unsigned long long> total_mem_used;

        // unsigned int infoCount = 0;
        // nvmlProcessInfo_t *infos = nullptr;
        unsigned int infoCount = 1024;
        nvmlProcessInfo_t* infos = new nvmlProcessInfo_t[infoCount]();
        std::vector<unsigned int> pids;
        if (nvml_ret != NVML_SUCCESS) {
            spdlog::error("nvmlDeviceGetHandleByIndex returned {}", nvmlErrorString(nvml_ret));
        }

        nvmlMemory_t memoryInfo;
        nvml_ret = nvmlDeviceGetMemoryInfo(nvml_dev, &memoryInfo);
        if (nvml_ret != NVML_SUCCESS) {
            spdlog::error("Failed to get memory info: {}", nvmlErrorString(nvml_ret));
            return;
        }

        if (i == 0 || gpu_memory == 0) {
            gpu_memory = memoryInfo.total;
        }

        nvml_ret = nvmlDeviceGetComputeRunningProcesses_v3(nvml_dev, &infoCount, infos);
        if (nvml_ret != NVML_SUCCESS && nvml_ret != NVML_ERROR_INSUFFICIENT_SIZE) {
            spdlog::error("nvmlDeviceGetComputeRunningProcesses_v3 failed with {}", nvmlErrorString(nvml_ret));
            spdlog::error("infoCount: {}", infoCount);
            return;
        }
        else if (nvml_ret == NVML_ERROR_INSUFFICIENT_SIZE) {
            spdlog::info("nvmlDeviceGetComputeRunningProcesses_v3 failed with {}. try again!", nvmlErrorString(nvml_ret));
            spdlog::info("infoCount: {}", infoCount);
            infoCount = 1024 * 10;
            // Allocate enough memory
            infos = new nvmlProcessInfo_t[infoCount]();

            // Second call to nvmlDeviceGetComputeRunningProcesses_v3 to get process info
            nvml_ret = nvmlDeviceGetComputeRunningProcesses_v3(nvml_dev, &infoCount, infos);
            if (nvml_ret != NVML_SUCCESS) {
                spdlog::error("nvmlDeviceGetComputeRunningProcesses_v3 failed with {}", nvmlErrorString(nvml_ret));
                delete[] infos;
                return;
            }

        }

        spdlog::info("nvmlDeviceGetComputeRunningProcesses_v3:");
        for (int j = 0; infos[j].pid != 0; j++) {
            mem_record[infos[j].pid] = infos[j].usedGpuMemory;
            spdlog::info("pid is: {}, usedGpuMemory is: {}", infos[j].pid, infos[j].usedGpuMemory);
        }

        unsigned int processSamplesCount = 1024;

        // Allocate buffer
        std::vector<nvmlProcessUtilizationSample_t> utilization(processSamplesCount);

        // Get utilization information
        nvml_ret = nvmlDeviceGetProcessUtilization(nvml_dev, utilization.data(), &processSamplesCount, 0);
        if (nvml_ret != NVML_SUCCESS) {
            spdlog::error("Failed to get process utilization: {}", nvmlErrorString(nvml_ret));
            // nvmlShutdown();
            error_utilization = 1;
        }

        spdlog::info("");

        // Process utilization information
        for (int k = 0; infos[k].pid != 0; k++) {
            std::string pod_uid = "";
            std::string docker_id = "";
            spdlog::info("");
            spdlog::info("pid is: {}", infos[k].pid);
            if (read_proc_cgroup(infos[k].pid, pod_uid, docker_id) != -1) {
                docker_id = docker_id.substr(0, 12);
                get_docker_gpus(docker_id);

                // Get pod_id
                std::string pod_id;
                if (pod_uid_to_id.find(pod_uid) == pod_uid_to_id.end()) {
                    std::string get_pod_id_cmd = "docker ps -a | grep " + docker_id + " | awk '{print $NF}' | awk -F '_' '{print $3}'";
                    if (get_pod_id(get_pod_id_cmd, pod_id) != 0) {
                        spdlog::error("exec {} failed", get_pod_id_cmd);
                        continue;
                    }
                    pod_uid_to_id[pod_uid] = pod_id;
                }
                else {
                    pod_id = pod_uid_to_id[pod_uid];
                }

                if (pod_id_to_docker_id.find(pod_id) == pod_id_to_docker_id.end()) {
                    pod_id_to_docker_id[pod_id] = docker_id;
                }

                if (orginal_gpu_data.find(pod_id) == orginal_gpu_data.end()) {
                    spdlog::warn("pod_id is not in orginal_gpu_data: {}", pod_id);
                    continue;
                }

                spdlog::info("gpu_index[pod_id].size(): {}", gpu_index[pod_id].size());
                if (gpu_index[pod_id].size() == 0 || gpu_index.find(pod_id) == gpu_index.end()) {
                    std::string get_gpus_in_container_cmd = "docker exec -i " + docker_id + " find /dev -name 'nvidia[0-9]*' | sort -V";
                    if (get_gpu_id_in_pod(get_gpus_in_container_cmd, pod_id, gpu_data) != 0) {
                        spdlog::error("exec {} failed", get_gpus_in_container_cmd);
                        continue;
                    }
                }
                spdlog::info("gpu_index[pod_id].size(): {}", gpu_index[pod_id].size());
                if (gpu_index[pod_id].size() == 0) {
                    continue;
                }
                // Accumulate previous statistics
                if (gpu_data.find(pod_id) == gpu_data.end()) {
                    // Initialize if pod_id does not exist
                    gpu_data[pod_id][gpu_index[pod_id][gpu_ids[i]]] = std::make_pair(0, 0);
                }
                if (total_gpu_util.find(pod_id) == total_gpu_util.end()) {
                    total_gpu_util[pod_id] = 0;
                }
                if (total_mem_used.find(pod_id) == total_mem_used.end()) {
                    total_mem_used[pod_id] = 0;
                }
                // Accumulate smUtil and mem_used
                total_mem_used[pod_id] += infos[k].usedGpuMemory;
                spdlog::info("pid is: {}, usedGpuMemory is: {}", infos[k].pid, infos[k].usedGpuMemory);

                if (error_utilization != 1) {
                    for (int j = 0; utilization[j].pid != 0; j++) {
                        if (utilization[j].pid == infos[k].pid) {
                            total_gpu_util[pod_id] += utilization[j].smUtil;
                            spdlog::info("pid is: {}, smUtil is: {}", infos[k].pid, utilization[j].smUtil);
                        }
                    }
                }

                spdlog::info("pod id is: {}, total_gpu_util is: {}, total_mem_used is: {}", pod_id, total_gpu_util[pod_id], total_mem_used[pod_id]);

                spdlog::info("gpu_ids[i] id is: {}, gpu id is: {}, gpu id in pod is: {}", gpu_ids[i], i, gpu_index[pod_id][gpu_ids[i]]);

                gpu_data[pod_id][gpu_index[pod_id][gpu_ids[i]]].first = total_gpu_util[pod_id];
                gpu_data[pod_id][gpu_index[pod_id][gpu_ids[i]]].second = total_mem_used[pod_id];
                spdlog::info("gpu_data[pod_id][gpu_index[pod_id][gpu_ids[i]]].first is: {}, gpu_data[pod_id][gpu_index[pod_id][gpu_ids[i]]].second is: {}", gpu_data[pod_id][gpu_index[pod_id][gpu_ids[i]]].first, gpu_data[pod_id][gpu_index[pod_id][gpu_ids[i]]].second);
            }
        }
        delete[] infos;
    }
}

// Function to expose gpu_data to Prometheus
void expose_gpu_data(std::shared_ptr<prometheus::Registry> registry,
    std::map<std::string, std::map<unsigned int, std::pair<unsigned int, unsigned long long>>>& gpu_data,
    prometheus::Family<prometheus::Gauge>& gauge_family_first,
    prometheus::Family<prometheus::Gauge>& gauge_family_second,
    prometheus::Family<prometheus::Gauge>& gauge_family_third)
{

    // Iterate through gpu_data and create metrics for each Pod's GPU utilization and memory usage
    for (const auto& pod : gpu_data) {
        const auto& pod_id = pod.first;
        int gpu_num = 0;
        for (const auto& gpu : pod.second) {
            unsigned int gpu_id = 1024;
            for (auto& index : gpu_index[pod_id]) {
                if (index.second == gpu.first) {
                    gpu_id = gpu.first;
                    gpu_num++;
                    break;
                }
            }

            if (gpu_id == 1024) {
                gpu_id = gpu_num;
                gpu_num++;
            }

            unsigned int sm_util = gpu.second.first * GPUAllocation;
            unsigned long long mem_used = gpu.second.second / 1024 / 1024;
            unsigned long long total_mem = gpu_memory / 1024 / 1024 / GPUAllocation;

            // Output metrics
            spdlog::info("Pod ID: {}, GPU ID: {}, SM Utilization: {}, Memory Used: {}, Total Memory: {}", pod_id, gpu_id, sm_util, mem_used, total_mem);

            // Find and update SM Util Gauge (using pod_id and gpu_id)
            prometheus::Gauge& sm_util_gauge = gauge_family_first.Add({ {"pod", pod_id}, {"gpu_id", std::to_string(gpu_id)} });
            sm_util_gauge.Set(sm_util);

            // Find and update Memory Used Gauge (using pod_id and gpu_id)
            prometheus::Gauge& mem_used_gauge = gauge_family_second.Add({ {"pod", pod_id}, {"gpu_id", std::to_string(gpu_id)} });
            mem_used_gauge.Set(mem_used);

            // Find and update Total Memory Gauge (using pod_id and gpu_id)
            prometheus::Gauge& total_mem_gauge = gauge_family_third.Add({ {"pod", pod_id}, {"gpu_id", std::to_string(gpu_id)} });
            total_mem_gauge.Set(total_mem);
        }
    }
}

size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* output)
{
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}

int get_pods(std::map<std::string, std::map<unsigned int, std::pair<unsigned int, unsigned long long>>>& gpu_data,
    CURL* curl, struct curl_slist* headers, const char* kubernetesServiceHost, const char* kubernetesServicePort, const std::string& currentNodeName, ondemand::parser& parser)
{
    std::string podsResponse;
    std::string podsUrl = "https://" + std::string(kubernetesServiceHost) + ":" + std::string(kubernetesServicePort) + "/api/v1/pods?fieldSelector=spec.nodeName=" + std::string(currentNodeName);

    curl_easy_setopt(curl, CURLOPT_URL, podsUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &podsResponse);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        spdlog::error("curl_easy_perform() failed: {}", curl_easy_strerror(res));
        return -1;
    }

    padded_string json = padded_string(podsResponse);
    ondemand::document info;
    auto error = parser.iterate(json).get(info);
    if (error) {
        spdlog::error("Error parsing JSON: {}", error_message(error));
        return -1;
    }

    ondemand::object pod_list;
    error = info.get_object().get(pod_list);
    if (error) {
        spdlog::error("Error getting pod list object: {}", error_message(error));
        return -1;
    }

    ondemand::array items;
    error = pod_list["items"].get_array().get(items);
    if (error) {
        spdlog::error("Error getting items array: {}", error_message(error));
        return -1;
    }

    for (auto pod_value : items) {
        ondemand::object pod;
        error = pod_value.get_object().get(pod);
        if (error) {
            spdlog::error("Error getting pod object: {}", error_message(error));
            continue;
        }

        ondemand::object spec;
        error = pod["spec"].get_object().get(spec);
        if (error) {
            spdlog::error("Error getting spec object: {}", error_message(error));
            continue;
        }
        ondemand::array containers;
        error = spec["containers"].get_array().get(containers);
        if (error) {
            spdlog::error("Error getting containers object: {}", error_message(error));
            continue;
        }
        int use_gpu = 0;
        std::string_view gpu_num_string;
        // Iterate through containers
        for (auto container_value : containers) {
            ondemand::object container;
            error = container_value.get_object().get(container);
            if (error) {
                spdlog::error("Error getting container object: {}", error_message(error));
                continue;
            }

            std::string_view container_name;
            error = container["name"].get(container_name);
            if (error) {
                spdlog::error("Error getting container name: {}", error_message(error));
                continue;
            }

            // std::cout << "Container Name: " << container_name << std::endl;

            // Get resources
            ondemand::object resources;
            error = container["resources"].get_object().get(resources);
            if (error) {
                spdlog::error("No resources defined for container: {}", container_name);
                continue;
            }

            // Check if nvidia.com/gpu is in limits
            ondemand::object limits;
            error = resources["limits"].get_object().get(limits);
            if (!error) {
                std::string_view limits_gpu;
                error = limits["nvidia.com/gpu"].get_string().get(limits_gpu);
                if (!error) {
                    use_gpu = 1;
                    gpu_num_string = limits_gpu;
                    // std::cout << "  GPU Limit: " << limits_gpu << std::endl;
                    break;
                }
                else {
                    // std::cerr << "No GPU limit defined for container: " << container_name << std::endl;
                    continue;
                }
            }
            else {
                // std::cerr << "No limits defined for container: " << container_name << std::endl;
                continue;
            }

            // Check if nvidia.com/gpu is in requests
            ondemand::object requests;
            error = resources["requests"].get_object().get(requests);
            if (!error) {
                std::string_view requests_gpu;
                error = requests["nvidia.com/gpu"].get_string().get(requests_gpu);
                if (!error) {
                    use_gpu = 1;
                    gpu_num_string = requests_gpu;
                    // std::cout << "  GPU Request: " << requests_gpu << std::endl;
                    break;
                }
                else {
                    // std::cerr << "No GPU request defined for container: " << container_name << std::endl;
                    continue;
                }
            }
            else {
                // std::cerr << "No requests defined for container: " << container_name << std::endl;
                continue;
            }
        }

        if (use_gpu == 0) {
            continue;
        }

        ondemand::object metadata;
        error = pod["metadata"].get_object().get(metadata);
        if (error) {
            spdlog::error("Error getting metadata object: {}", error_message(error));
            continue;
        }

        std::string_view name;
        error = metadata["name"].get(name);
        if (error) {
            spdlog::error("Error getting pod name: {}", error_message(error));
            continue;
        }

        std::string_view namespace_;
        error = metadata["namespace"].get(namespace_);
        if (error) {
            spdlog::error("Error getting pod namespace: {}", error_message(error));
            continue;
        }

        ondemand::object status;
        error = pod["status"].get_object().get(status);
        if (error) {
            spdlog::error("Error getting status object: {}", error_message(error));
            continue;
        }

        std::string_view phase;
        error = status["phase"].get(phase);
        if (error) {
            spdlog::error("Error getting pod phase: {}", error_message(error));
            continue;
        }

        int gpu_num = std::stoi(std::string(gpu_num_string));
        std::string podname = std::string(name);

        for (int i = 0; i < gpu_num; i++) {
            if (gpu_data[podname].find(i) == gpu_data[podname].end()) {
                gpu_data[podname][i] = std::make_pair(0, 0);
            }
        }
    }

    return 0;
}

// Periodically clean and update GPU data
void update_and_clean_gpu_data(std::map<std::string, std::map<unsigned int, std::pair<unsigned int, unsigned long long>>>& gpu_data,
    std::map<std::string, std::map<unsigned int, std::time_t>>& last_update_times, nvmlDevice_t nvml_dev, unsigned int device_count,
    std::shared_ptr<prometheus::Registry> registry,
    std::chrono::time_point<std::chrono::high_resolution_clock> start,
    prometheus::Family<prometheus::Gauge>& gauge_family_first, prometheus::Family<prometheus::Gauge>& gauge_family_second, prometheus::Family<prometheus::Gauge>& gauge_family_third)
{
    const int expiry_time = 10; // Set data expiry time to 10 seconds
    std::time_t current_time = std::time(nullptr);

    // Update GPU data
    std::map<std::string, std::map<unsigned int, std::pair<unsigned int, unsigned long long>>> new_gpu_data, adusted_gpu_data;
    get_usuage(gpu_data, new_gpu_data, nvml_dev, device_count, start);

    for (const auto& pod : new_gpu_data) {
        int new_index = 0;
        for (auto& gpu_item : pod.second) {
            unsigned int original_index = gpu_item.first;
            unsigned int util = gpu_item.second.first;
            unsigned long long memory = gpu_item.second.second;

            if(pod_id_to_docker_id.find(pod.first) == pod_id_to_docker_id.end()) {
                continue;
            }
            if (gpu_usage.find(pod_id_to_docker_id.at(pod.first)) != gpu_usage.end() && gpu_usage.at(pod_id_to_docker_id.at(pod.first)).find(original_index) != gpu_usage.at(pod_id_to_docker_id.at(pod.first)).end()) {
                std::vector<unsigned long long> split_values = split_data(memory, gpu_usage.at(pod_id_to_docker_id.at(pod.first)).at(original_index));
                for (unsigned long long split_memory : split_values) {
                    adusted_gpu_data[pod.first][new_index++] = std::make_pair(util, split_memory);
                }
            }
        }
    }

    // Iterate through new data and update last_update_times
    for (const auto& pod : adusted_gpu_data) {
        for (auto& gpu_item : pod.second) {
            last_update_times[pod.first][gpu_item.first] = current_time;
            spdlog::info("pod_id is: {}, gpu_id is: {}", pod.first, gpu_item.first);
            gpu_data[pod.first][gpu_item.first] = gpu_item.second;
        }
    }

    // Clean up old data: check inactive Pods and zero out their data
    for (auto it = gpu_data.begin(); it != gpu_data.end();) {
        spdlog::info("pod_id is: {}", it->first);
        const auto& pod_id = it->first;
        for (auto& gpu_item : it->second) {
            if (last_update_times[pod_id].find(gpu_item.first) == last_update_times[pod_id].end() || last_update_times[pod_id][gpu_item.first] + expiry_time < current_time) {
                gpu_item.second = { 0, 0 }; // Zero out GPU utilization data
            }
        }
        ++it;
    }

    // Output cleaned data
    for (const auto& pod_item : gpu_data) {
        spdlog::info("Pod ID: {}", pod_item.first);
        for (const auto& gpu_item : pod_item.second) {
            spdlog::info("  GPU ID: {}, Utilization: {}, Memory: {}", gpu_item.first, gpu_item.second.first, gpu_item.second.second);
        }
    }

    // Update gpu_data to Prometheus
    expose_gpu_data(registry, gpu_data, gauge_family_first, gauge_family_second, gauge_family_third);
}

int main()
{
    init_logger();
    auto start = std::chrono::high_resolution_clock::now();
    nvmlDevice_t nvml_dev;
    nvmlReturn_t result;
    std::map<int, unsigned long long> mem_record;
    unsigned int device_count;
    std::map<std::string, std::map<unsigned int, std::pair<unsigned int, unsigned long long>>> gpu_data;

    // Create a Prometheus exposer
    prometheus::Exposer exposer{ "0.0.0.0:8080" };

    // Create a Prometheus registry
    auto registry = std::make_shared<prometheus::Registry>();

    auto& gauge_family_first = prometheus::BuildGauge()
        .Name("pod_gpu_sm_util")
        .Help("GPU SM utilization per pod")
        .Register(*registry);

    auto& gauge_family_second = prometheus::BuildGauge()
        .Name("pod_gpu_memory_used")
        .Help("GPU memory usage per pod")
        .Register(*registry);

    auto& gauge_family_third = prometheus::BuildGauge()
        .Name("pod_total_gpu_memory")
        .Help("Total GPU memory per pod")
        .Register(*registry);

    // Register the registry with the Exposer
    exposer.RegisterCollectable(registry);

    // Read GPU allocation ratio
    if (read_allocation() != 0) {
        return -1;
    }

    result = nvmlInit();
    if (result != NVML_SUCCESS) {
        spdlog::error("nvmlInit failed with {}", nvmlErrorString(result));
        return -1;
    }

    result = nvmlDeviceGetCount(&device_count);
    if (NVML_SUCCESS != result) {
        spdlog::error("Failed to query device count: {}", nvmlErrorString(result));
        return -1;
    }
    spdlog::info("Current device count is: {}", device_count);

    get_gpu_uuids();

    // Maintain a record of last update times
    std::map<std::string, std::map<unsigned int, std::time_t>> last_update_times;

    std::ifstream tokenFile("/var/run/secrets/kubernetes.io/serviceaccount/token");
    std::string token;
    if (tokenFile.is_open()) {
        std::getline(tokenFile, token);
        tokenFile.close();
    }
    else {
        spdlog::error("Error opening token file");
        return -1;
    }

    char* kubernetesServiceHost = std::getenv("KUBERNETES_SERVICE_HOST");
    if (!kubernetesServiceHost) {
        spdlog::error("Error: KUBERNETES_SERVICE_HOST environment variable not set.");
        return -1;
    }

    char* kubernetesServicePort = std::getenv("KUBERNETES_SERVICE_PORT");
    if (!kubernetesServicePort) {
        spdlog::error("Error: KUBERNETES_SERVICE_PORT environment variable not set.");
        return -1;
    }

    char* nodeNameEnv = std::getenv("CURRENT_NODE_NAME");
    if (!nodeNameEnv) {
        spdlog::error("Error: CURRENT_NODE_NAME environment variable not set.");
        return -1;
    }
    std::string currentNodeName(nodeNameEnv);

    if (!kubernetesServiceHost || !kubernetesServicePort || !nodeNameEnv) {
        spdlog::error("Error: Environment variables not set");
        return -1;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        spdlog::error("Error initializing curl");
        return -1;
    }
    std::string header = "Authorization: Bearer " + token;
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Connection: Keep-Alive"); // Add Keep-Alive header
    headers = curl_slist_append(headers, header.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    // curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); // Total timeout 5 seconds
    // curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L); // Connection timeout 3 seconds

    ondemand::parser parser;

    // Periodically update and clean GPU data
    while (true) {
        get_pods(gpu_data, curl, headers, kubernetesServiceHost, kubernetesServicePort, currentNodeName, parser);

        update_and_clean_gpu_data(gpu_data, last_update_times, nvml_dev, device_count, registry, start, gauge_family_first, gauge_family_second, gauge_family_third);

        // Update every 5 seconds
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    // Cleanup
    nvmlShutdown();

    return 0;
}
