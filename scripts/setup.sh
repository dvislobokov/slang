#!/bin/sh
# Development dependencies for building the Zell compiler (Ubuntu 24.04).
set -e
apt-get update
apt-get install -y llvm-18-dev libpolly-18-dev libzstd-dev clang-18 lld-18
