# 构建、平台和打包

## 平台范围

| 目标 | 核心网络实现 | 服务接入 | 说明 |
| --- | --- | --- | --- |
| Windows 10 / 11、现代 Windows Server | WinHTTP + IP Helper | Windows SCM | 本机可生成 `.exe`；安装脚本需管理员 PowerShell 5.1+ |
| Linux x86_64 / ARM64 等 | libcurl + getifaddrs | systemd / OpenRC | 使用对应架构工具链；动态链接库需安装在目标系统 |
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

使用[CPack](https://cmake.org/cmake/help/latest/manual/cpack.1.html)为当前构建目标打包。包中的二进制仍受操作系统、CPU 架构、libc 和 TLS 动态依赖限制；在较新 Linux 构建的程序不保证能在较旧 glibc 系统运行。

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
