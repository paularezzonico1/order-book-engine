# ---------------------------------------------------------------------------
# Reproducible Linux build/test/profile environment for order-book-engine.
#
#   docker build -t obe .
#   docker run --rm obe                      # runs the test suite (default)
#   docker run --rm obe ./build/obe_bench    # runs the benchmark
#   docker run --rm obe bench/sweep.sh build/obe_bench   # sweep -> CSV
#
# valgrind is included so the documented callgrind profiling pass can be
# reproduced on a clean Linux box (see PROFILING.md).
# ---------------------------------------------------------------------------
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        ca-certificates \
        valgrind \
        linux-tools-generic \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy build inputs. (Build artifacts are excluded via .dockerignore.)
COPY . .

# Configure + build the Release tree (engine, benchmark, tests). FetchContent
# pulls GoogleTest during configure, so the image is self-contained.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j "$(nproc)"

# Default command: run the test suite via ctest.
CMD ["ctest", "--test-dir", "build", "--output-on-failure"]
