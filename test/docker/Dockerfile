FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG LLVM_VERSION=20

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gnupg \
    lsb-release \
    software-properties-common \
    build-essential \
    cmake \
    ninja-build \
    python3-venv \
    python3 \
    git \
    && rm -rf /var/lib/apt/lists/*

# Install LLVM/Clang toolchain (retried: apt.llvm.org has short outages)
COPY scripts/ci/install-llvm-apt.sh /tmp/install-llvm-apt.sh
RUN bash /tmp/install-llvm-apt.sh ${LLVM_VERSION} libclang-${LLVM_VERSION}-dev \
    && rm -f /tmp/install-llvm-apt.sh \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /repo
COPY . /repo

RUN rm -rf build \
    && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
       -DLLVM_DIR=/usr/lib/llvm-${LLVM_VERSION}/lib/cmake/llvm \
       -DClang_DIR=/usr/lib/llvm-${LLVM_VERSION}/lib/cmake/clang \
       -DCLANG_LINK_CLANG_DYLIB=ON \
       -DLLVM_LINK_LLVM_DYLIB=ON \
       -DUSE_SHARED_LIB=OFF \
    && cmake --build build -j"$(nproc)"

RUN bash test/scripts/linux_compile.sh \
    && bash test/run_autofree_tests.sh /tmp/ct_autofree_tests \
    && bash test/run_runtime_tests.sh /tmp/ct_runtime_tests

RUN python3 -m venv .venv \
    && . .venv/bin/activate \
    && python -m pip install --upgrade pip \
    && python -m pip install git+https://github.com/CoreTrace/coretrace-testkit.git \
    && python test/examples/test_smoke.py \
    && python test/examples/test_extern_project.py \
    && python test/examples/test_help_smoke.py
