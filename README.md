# IP Reporter 客户端

C++17 客户端读取机器的网卡 ID、IPv4、IPv6 和 MAC，并通过 HTTPS 上报。支持前台运行和系统服务。服务端与部署脚本在独立项目中维护，客户端仓库可直接作为 GitHub 仓库根目录使用。

## 获取安装包

从本仓库的 **Releases** 选择对应系统和架构的 ZIP 或 TGZ。DEB 与 RPM 用于通过包管理器安装。每个发行版都带有 SHA-256 清单，供管理员在服务器上镜像及设备安装时校验。服务器控制台提供从镜像下载、配置地址和注册服务的统一入口。

| 平台 | Release 包标识 |
| --- | --- |
| Windows x64 / ARM64 | `windows-x86_64` / `windows-arm64` |
| Ubuntu 22.04 x64 | `ubuntu22.04-x86_64` |
| Ubuntu 24.04 x64 / ARM64 | `ubuntu24.04-x86_64` / `ubuntu24.04-arm64` |
| Debian 12 / 13 x64 | `debian12-x86_64` / `debian13-x86_64` |
| Rocky Linux 9 x64 | `rocky9-x86_64` |
| Alpine Linux 3.22 x64 | `alpine3.22-x86_64` |
| macOS 13+ Intel / Apple Silicon | `macos-x86_64` / `macos-arm64` |

Linux 按发行版及架构分别构建。其他系统和版本可[从源码构建](docs/build.md)，FreeBSD 可以使用附带的 rc.d 安装脚本。

## 注册服务

使用控制台提供的安装命令，或者解压对应 Release 包后运行包内的安装脚本。将示例域名和 Token 文件路径换成实际配置。

在管理员 PowerShell 中，进入 Windows ZIP 解压后的首层目录：

```powershell
& .\share\ip-reporter\scripts\install.ps1 -Endpoint 'https://report.example.com/api/report' -TokenFile 'C:\secure\report-token.txt'
```

在 Linux，进入 TGZ 解压后的首层目录：

```sh
sudo sh usr/share/ip-reporter/scripts/install.sh --endpoint https://report.example.com/api/report --token-file /root/report-token.txt
```

macOS 使用 `share/ip-reporter/scripts/install.sh`。Token 文件只包含上报 Token 一行。已安装设备重新运行安装脚本时保留原配置；需要更换上报地址或 Token 时加 `--reconfigure` 或 `-Reconfigure`。1.1.2 起重新配置默认继承已有设备 ID，显式指定新 ID 才更换身份；其他参数按本次输入重建，需要认证时仍须提供 Token 文件。

上报接口见[协议说明](docs/protocol.md)，手动安装、更新、排错和卸载见[运维说明](docs/operations.md)与[包使用说明](packaging/README.md)。

## 构建与发布

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cpack --config build/CPackConfig.cmake -C Release
```

GitHub Actions 在分支和 PR 上构建并测试，产物可在 Actions 运行记录中下载。正式版本使用与 `CMakeLists.txt` 一致的 `vX.Y.Z` 标签。所有平台通过后，CI 上传安装包、`INSTALL-INDEX.json` 和 `SHA256SUMS.txt`，再发布 Release。发布步骤与清单格式见[GitHub 发布说明](docs/releasing.md)。
