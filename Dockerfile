FROM --platform=linux/arm64 ubuntu:24.04

RUN apt-get update -qq && \
    apt-get install -y -qq --no-install-recommends \
      g++ make protobuf-compiler libprotobuf-dev libspdlog-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
