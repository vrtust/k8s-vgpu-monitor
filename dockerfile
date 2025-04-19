FROM nvidia/cuda:12.1.0-base-ubuntu22.04 AS build

WORKDIR /workspace

COPY ./prometheus-cpp /workspace/prometheus-cpp

RUN apt update -y && \
    apt install -y \
    g++ \
    cmake && \
    apt clean all

RUN cd prometheus-cpp && \
    rm -rf _build &&\
    mkdir _build && \
    cd _build && \
    cmake .. -DBUILD_SHARED_LIBS=ON -DENABLE_PUSH=OFF -DENABLE_COMPRESSION=OFF && \
    cmake --build . --parallel 4 && \
    ctest -V && \
    cmake --install . && \
    ldconfig

FROM nvidia/cuda:12.1.0-base-ubuntu22.04

WORKDIR /workspace

COPY --from=build /usr/local/lib /usr/local/lib
COPY --from=build /usr/local/include /usr/local/include

RUN apt update -y && \
    apt install -y \
    libcurl4-openssl-dev && \
    apt clean all

COPY build/vgpu_monitor /workspace/

# CMD ["./vgpu_monitor"]