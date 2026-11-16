# minidb 개발 컨테이너: 빌드·테스트·sanitizer·벤치마크를 모두 여기서 돌린다.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build gdb libgtest-dev time ca-certificates \
        pkg-config libcpp-httplib-dev curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["bash"]
