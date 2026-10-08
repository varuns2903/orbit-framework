# Drogon, pinned, built in Release. Build jobs are capped (BUILD_JOBS) so the
# image builds on small machines.
FROM ubuntu:24.04
ARG DROGON_VERSION=v1.9.13
ARG BUILD_JOBS=2
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates g++ cmake make git libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch "${DROGON_VERSION}" --recurse-submodules \
        https://github.com/drogonframework/drogon.git /src/drogon \
    && cmake -S /src/drogon -B /src/drogon/build -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_EXAMPLES=OFF -DBUILD_CTL=OFF -DBUILD_ORM=OFF \
    && cmake --build /src/drogon/build -j"${BUILD_JOBS}" \
    && cmake --install /src/drogon/build \
    && rm -rf /src/drogon
COPY main.cc /src/bench/main.cc
RUN printf 'cmake_minimum_required(VERSION 3.16)\nproject(drogon_bench CXX)\nset(CMAKE_CXX_STANDARD 17)\nfind_package(Drogon CONFIG REQUIRED)\nadd_executable(drogon_bench main.cc)\ntarget_link_libraries(drogon_bench PRIVATE Drogon::Drogon)\n' > /src/bench/CMakeLists.txt \
    && cmake -S /src/bench -B /src/bench/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /src/bench/build -j"${BUILD_JOBS}" \
    && cp /src/bench/build/drogon_bench /usr/local/bin/
ENTRYPOINT ["/usr/local/bin/drogon_bench"]
