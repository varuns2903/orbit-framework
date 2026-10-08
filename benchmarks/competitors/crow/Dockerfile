# Crow (header-only), pinned, built with -O2.
FROM ubuntu:24.04
ARG CROW_VERSION=v1.3.5
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates g++ git libasio-dev \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch "${CROW_VERSION}" https://github.com/CrowCpp/Crow.git /src/crow
COPY main.cpp /src/bench/main.cpp
RUN g++ -std=c++17 -O2 -DNDEBUG -I/src/crow/include /src/bench/main.cpp -o /usr/local/bin/crow_bench -pthread
ENTRYPOINT ["/usr/local/bin/crow_bench"]
