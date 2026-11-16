# 第三方组件

- **nlohmann/json 3.12.0**：MIT，源文件 `third_party/nlohmann/json.hpp`，许可证 `third_party/nlohmann/LICENSE.MIT`。来自上游固定版本：https://github.com/nlohmann/json/tree/v3.12.0 。头文件 SHA-256：`aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`。
- **libcurl**（POSIX 构建）：采用 curl 许可证，参见 https://curl.se/docs/copyright.html 。普通 POSIX 包使用系统动态库，实际 TLS 后端及其许可证由目标系统的软件包决定。
- **musl 静态 Linux 发布包**：由 Alpine Linux 3.22 软件包链入 musl、GCC C++/运行库、libcurl、OpenSSL、c-ares、nghttp2、libidn2、libunistring、libpsl、Brotli、zlib 和 Zstandard。其许可证分别包括 MIT/curl、Apache-2.0、LGPL-3.0-or-later、GPL-3.0-with-GCC-exception、BSD-3-Clause 和 Zlib；以构建时 Alpine 包元数据与上游源码为准。对应源码及许可文本：https://gitlab.alpinelinux.org/alpine/aports/-/tree/3.22-stable/main 、https://curl.se/docs/copyright.html 、https://www.openssl.org/source/license.html 、https://musl.libc.org/copyright.html 、https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html 。
- **Windows SDK / WinHTTP**：调用 Windows 自带 API，不附带第三方网络运行库。

分发程序时保留本文件、项目 LICENSE 和 nlohmann/json 的许可证。若改变静态依赖或其构建选项，请重新核对相应许可证、源码及可重链接要求。
