# 构建、平台和打包

## 平台范围

| 目标 | 核心网络实现 | 服务接入 | 说明 |
| --- | --- | --- | --- |
| Windows 10 / 11、现代 Windows Server | WinHTTP + IP Helper | Windows SCM | 本机可生成 `.exe`；安装脚本需管理员 PowerShell 5.1+ |
| Linux x86_64 / ARM64 等 | libcurl + getifaddrs | systemd / OpenRC | 普通包需匹配目标发行版；`linux-musl-*` 为全静态通用 Linux ELF |
| macOS Intel / Apple Silicon | libcurl + getifaddrs | launchd | 分别编译 x86_64 / arm64，或自行制作 Universal Binary |
| FreeBSD | libcurl + getifaddrs | rc.d | 提供实现和脚本，需在 FreeBSD 目标环境验证 |

这里列出的是实现目标，不是所有版本的实机认证。默认不承诺 Windows 7、旧版 macOS、Android / iOS、裸机、其他 Unix 或无服务管理器容器兼容。容器可以以前台模式运行程序，由容器编排系统负责重启，通常不应在容器内安装系统服务。

## 通用依赖

- CMake 3.20+。
- 支持 C++17 标准库和 `std::filesystem` 的编译器，如 Visual Studio 2022、GCC 9+、较新的 Clang / AppleClang。
- Windows：Windows SDK；网络模块链接系统库，不需下载 libcurl。
- POSIX：libcurl 7.34+ 开发包和可用的 TLS 后端 / CA 证书库；部署时优先使用发行版维护的当前安全版本。
- nlohmann/json 已随项目固定版本提供，无需构建时联网获取。第三方版权见 `THIRD_PARTY_NOTICES.md`。
- Python 3.10+ 为可选开发依赖，用于集成测试和接收示例。

### Windows

安装 Visual Studio 2022 的“使用 C++ 的桌面开发”工作负载，或对应 Build Tools。在开发者终端中：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

输出为 `build\Release\ip-reporter.exe`（使用单配置生成器时可能为 `build\ip-reporter.exe`）。项目对 MSVC 使用静态 C / C++ 运行库；目标操作系统仍需提供 WinHTTP 等系统组件。

ARM64 可在安装对应工具链后使用新的构建目录并指定 `-A ARM64`。交叉编译出来的程序不能自动在构建机执行测试，应在目标设备测试。

### Debian / Ubuntu

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake libcurl4-openssl-dev ca-certificates python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Fedora / RHEL 系

```sh
sudo dnf install -y gcc-c++ cmake libcurl-devel ca-certificates python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Alpine / OpenRC

```sh
sudo apk add build-base cmake curl-dev ca-certificates python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### musl 全静态 Linux / 群晖

发布矩阵在 Alpine musl 容器内原生构建 `linux-musl-x86_64` 和 `linux-musl-arm64`。该产物把 musl、C++ 运行库、libcurl、OpenSSL 及 libcurl 的传递依赖都链入单一 ELF；CI 会拒绝任何带 `PT_INTERP` 或 `DT_NEEDED` 的所谓“静态”产物。本地复现构建：

```sh
docker run --rm -e STATIC_BUILD=true -v "$PWD:/source" -w /source alpine:3.22 \
  sh ci/build-linux.sh "linux-musl-$(uname -m | sed 's/aarch64/arm64/')" ""
```

也可在已备齐静态 libcurl 及其依赖的 musl 工具链中直接使用 CMake：

```sh
cmake -S . -B build-static -DCMAKE_BUILD_TYPE=Release \
  -DIP_REPORTER_STATIC=ON -DIP_REPORTER_PACKAGE_PLATFORM=linux-musl-x86_64
cmake --build build-static --parallel
```

群晖使用时先在 SSH 中用 `uname -m` 确认架构：`x86_64` 选 x86_64 包，`aarch64` 选 ARM64 包。解压后可先执行 `usr/bin/ip-reporter --version`，再按 README 运行 `usr/share/ip-reporter/scripts/install.sh`。安装器只能直接注册它能识别的 systemd/OpenRC；若 DSM 不暴露其中一种管理器，请把前台命令 `/usr/local/bin/ip-reporter --config /etc/ip-reporter/config.json` 纳入 DSM 的套件/服务生命周期，不要伪装成 SPK 上传。群晖官方的 [DSM 套件脚本规范](https://help.synology.com/developer-guide/synology_package/scripts.html) 要求使用 `start-stop-status` 管理启停，而官方[运行时说明](https://help.synology.com/developer-guide/getting_started/system_requirement.html) 明确指出 DSM 6/7 套件不互相兼容；要在“套件中心”手动安装，仍需另行制作并实机验证 SPK。

静态链接不会把公共 CA 证书嵌入程序。HTTPS 仍需目标设备提供 CA 库；程序会在尊重 `CURL_CA_BUNDLE` / `SSL_CERT_FILE` / `SSL_CERT_DIR` 管理员设置的前提下，依次查找 Alpine、Debian/Ubuntu/Synology DSM、RHEL 和 SUSE 的常见系统 CA 路径。同时需保持 DSM 系统时间正确。“全静态”解决用户态动态库 ABI，不代表所有 Linux 内核版本、CPU 子架构或 DSM 服务机制都已经实机认证。

### macOS

安装 Xcode Command Line Tools 和 CMake。使用已安装的 Homebrew 时可执行：

```sh
xcode-select --install
brew install cmake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

默认使用系统 SDK 的 libcurl。如自行指定 Homebrew curl，注意它可能引入额外动态库，分发时需要目标机也具有这些依赖。安装后可用 `otool -L build/ip-reporter` 检查。

### FreeBSD

```sh
sudo pkg install -y cmake curl python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 测试

```sh
ctest --test-dir build -C Release --output-on-failure
python -m unittest discover -s tools -p test_receiver.py -v
```

CTest 包含核心逻辑测试和可用时的 Python 端到端测试。端到端测试仅向本机临时端口发送报告，不应安装系统服务或修改真实系统配置。安装脚本应另在可恢复虚拟机内验证启动、停止、升级和卸载。

## 打包

使用[CPack](https://cmake.org/cmake/help/latest/manual/cpack.1.html)为当前构建目标打包。普通包中的二进制仍受操作系统、CPU 架构、libc 和 TLS 动态依赖限制；在较新 Linux 构建的程序不保证能在较旧 glibc 系统运行。`linux-musl-*` 例外，其全静态链接属性由 CI 额外验证。

```sh
# 通用归档
cpack --config build/CPackConfig.cmake -C Release -G TGZ

# Windows ZIP
cpack --config build/CPackConfig.cmake -C Release -G ZIP

# Debian 系；需要 dpkg 工具
cpack --config build/CPackConfig.cmake -C Release -G DEB

# RPM 系；需要 rpmbuild，例如 rpm-build 包
cpack --config build/CPackConfig.cmake -C Release -G RPM
```

若 RPM 打包遇到包含空格的构建 / 暂存路径，可从项目根目录指定不含空格的输出和暂存目录：

```sh
cpack --config "$(pwd)/build/CPackConfig.cmake" -C Release -G RPM -B /tmp/ip-reporter-rpm-packages
```

Linux 工件名带有构建发行版和架构，例如 `ubuntu24.04-x86_64`；可在配置时使用 `-DIP_REPORTER_PACKAGE_PLATFORM=...` 覆盖标签。标签不改变二进制 ABI，Ubuntu 上构建的 RPM 仍需在相应目标发行版重新构建才能保证其运行库兼容。

Windows 可执行文件本身就是编译产物，ZIP 中带有安装脚本。DEB / RPM 安装文件与首次配置服务是两个步骤：安装包不能预先知道你的接收地址和 Token。安装包后再运行其附带的 `install.sh` 完成初始化和服务注册；详见 `packaging/README.md`。本项目不附带虚构的下载地址，也不把未经生成验证的包标为已发布。

CPack 工件默认位于 `build/packages/`。Windows ZIP 顶层为 `bin/` 和 `share/`；Linux TGZ 顶层包含 `usr/bin/` 和 `usr/share/`；macOS TGZ 使用 `bin/` 和 `share/`。解压后先进入该包的公共顶层目录，再调用其中的安装脚本，不要假设归档包含源码目录中的根级 `scripts/`。

CI 的具体构建矩阵和 GitHub Release 发布方法见[发布说明](releasing.md)。CPack 对各生成器的要求见[官方打包说明](https://cmake.org/cmake/help/latest/manual/cpack-generators.7.html)。
