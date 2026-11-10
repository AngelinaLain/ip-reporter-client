#!/bin/sh
set -eu

platform=$1
native_format=${2:-}
build_dir=${BUILD_DIRECTORY:-build}
case "$platform" in
    ubuntu*|debian*)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends build-essential cmake libcurl4-openssl-dev ca-certificates python3 dpkg-dev
        ;;
    rocky9-*)
        dnf install -y gcc-c++ cmake libcurl-devel ca-certificates python3 make rpm-build
        ;;
    alpine*)
        apk add --no-cache build-base cmake curl-dev ca-certificates python3
        ;;
    *) printf 'Unknown Linux build platform: %s\n' "$platform" >&2; exit 2 ;;
esac

sh -n scripts/install.sh
sh -n scripts/uninstall.sh
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    "-DIP_REPORTER_PACKAGE_PLATFORM=$platform"
cmake --build "$build_dir" --parallel 2
ctest --test-dir "$build_dir" --output-on-failure --parallel 2
cpack --config "$build_dir/CPackConfig.cmake" -C Release -G TGZ
case "$native_format" in
    DEB|RPM) cpack --config "$build_dir/CPackConfig.cmake" -C Release -G "$native_format" ;;
    '') ;;
    *) printf 'Unknown native package format: %s\n' "$native_format" >&2; exit 2 ;;
esac
