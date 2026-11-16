#!/bin/sh
set -eu

platform=$1
native_format=${2:-}
build_dir=${BUILD_DIRECTORY:-build}
static_build=${STATIC_BUILD:-false}
case "$platform" in
    ubuntu*|debian*)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        # CPack 需要 file 识别 ELF 文件，才能生成 DEB 的运行时依赖。
        apt-get install -y --no-install-recommends build-essential cmake libcurl4-openssl-dev ca-certificates python3 dpkg-dev file
        ;;
    rocky9-*)
        dnf install -y gcc-c++ cmake libcurl-devel ca-certificates python3 make rpm-build
        ;;
    alpine*)
        apk add --no-cache build-base cmake curl-dev ca-certificates python3
        ;;
    linux-musl-*)
        [ "$static_build" = true ] || {
            printf 'Platform %s must be built with STATIC_BUILD=true.\n' "$platform" >&2
            exit 2
        }
        apk add --no-cache build-base cmake curl-dev curl-static ca-certificates python3 file \
            brotli-static libidn2-static libpsl-static libunistring-static nghttp2-static \
            openssl-libs-static zlib-static zstd-static
        ;;
    *) printf 'Unknown Linux build platform: %s\n' "$platform" >&2; exit 2 ;;
esac

case "$static_build" in
    true)
        case "$platform" in linux-musl-*) ;; *)
            printf 'STATIC_BUILD=true is reserved for linux-musl-* targets.\n' >&2
            exit 2 ;;
        esac ;;
    false|'') ;;
    *) printf 'STATIC_BUILD must be true or false.\n' >&2; exit 2 ;;
esac

sh -n scripts/install.sh
sh -n scripts/uninstall.sh
set -- -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    "-DIP_REPORTER_PACKAGE_PLATFORM=$platform"
if [ "$static_build" = true ]; then set -- "$@" -DIP_REPORTER_STATIC=ON; fi
cmake -S . -B "$build_dir" "$@"
cmake --build "$build_dir" --parallel 2
ctest --test-dir "$build_dir" --output-on-failure --parallel 2
if [ "$static_build" = true ]; then
    binary=$build_dir/ip-reporter
    strip "$binary"
    file "$binary" | grep -q 'statically linked' || {
        printf 'Static release target produced a non-static executable.\n' >&2
        exit 1
    }
    if readelf -l "$binary" | grep -q 'INTERP'; then
        printf 'Static release target contains a program interpreter.\n' >&2
        exit 1
    fi
    if readelf -d "$binary" | grep -q 'NEEDED'; then
        printf 'Static release target contains dynamic dependencies.\n' >&2
        exit 1
    fi
fi
cpack --config "$build_dir/CPackConfig.cmake" -C Release -G TGZ
case "$native_format" in
    DEB|RPM) cpack --config "$build_dir/CPackConfig.cmake" -C Release -G "$native_format" ;;
    '') ;;
    *) printf 'Unknown native package format: %s\n' "$native_format" >&2; exit 2 ;;
esac
