# Use Ubuntu 22.04 as the base image for a modern Linux kernel environment
FROM ubuntu:22.04

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build essentials, CMake, liburing development headers, and pkg-config
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    liburing-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Set the working directory
WORKDIR /usr/src/lsm_tree

# Copy the project files into the container
COPY . .

# Create a build directory, run CMake, and compile the project
RUN mkdir -p build && \
    cd build && \
    cmake .. && \
    make

# Set the entry point to run the tests
CMD ["./build/lsm_tests"]
