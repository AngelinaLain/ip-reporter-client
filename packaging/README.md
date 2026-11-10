# 打包与安装

打包文件包含可执行程序、安装/卸载脚本、服务模板和中文文档。原生包不会在缺少接收地址时自动启动服务。安装程序不会下载远程可执行文件，也不会绕过系统服务管理器。

## 生成安装包

成功编译到 `build` 目录后，在项目根目录运行：

```sh
cpack --config build/CPackConfig.cmake -C Release
# 以下两项需要 Linux 上对应的 dpkg/rpmbuild 工具。
cpack --config build/CPackConfig.cmake -C Release -G DEB
cpack --config build/CPackConfig.cmake -C Release -G RPM
```

`rpmbuild` 不支持包含空格的工作/暂存路径。如果源码或构建目录带空格，给 CPack 指定无空格的输出目录，并以绝对路径传入配置文件：

```sh
cpack --config "$(pwd)/build/CPackConfig.cmake" -C Release -G RPM -B /tmp/ip-reporter-rpm-packages
```

此时 RPM 及校验文件输出到 `/tmp/ip-reporter-rpm-packages/`。

输出位于 `build/packages/`，每个包附带 SHA-256 校验文件。Windows 默认 ZIP，macOS、Linux 默认 TGZ。GitHub Actions 按平台和架构编译、测试后，发布 ZIP/TGZ，并为 Debian/Ubuntu 目标构建 DEB，为 Rocky Linux 9 构建 RPM。依赖系统 libcurl 的 Unix 二进制需要与运行主机兼容的动态库及 C/C++ 运行时。

工件名包含构建平台与架构，例如 `ip-reporter-1.0.0-ubuntu24.04-x86_64.tar.gz`、`ip-reporter_1.0.0_ubuntu24.04-x86_64.deb`、`ip-reporter-1.0.0-ubuntu24.04-x86_64.rpm`。RPM 是包格式：在 Ubuntu 24.04 上构建的 RPM 仍依赖该环境的 glibc、libstdc++、libcurl/TLS ABI，不能视为 RHEL/Fedora 通用二进制。应在目标发行版或兼容构建容器中重新编译打包。交叉编译时通过 `-DIP_REPORTER_PACKAGE_PLATFORM=<平台-架构>` 指定标签。

Debian/Ubuntu 打包主机建议安装 `build-essential cmake libcurl4-openssl-dev dpkg-dev rpm`；其中 `dpkg-dev` 提供 `dpkg-shlibdeps`、`rpm` 提供 `rpmbuild`。Fedora/RHEL 对应构建依赖通常为 `gcc-c++ cmake libcurl-devel rpm-build`。DEB/RPM 生成器会扫描二进制的动态库依赖，接收机仍需通过包管理器满足这些依赖。

## Linux DEB / RPM

先安装包，再提供自己的接收地址来初始化服务；将示例地址替换为实际地址：

```sh
# Debian / Ubuntu
sudo apt install ./ip-reporter_*.deb
# Fedora / RHEL 家族
sudo dnf install ./ip-reporter-*.rpm

sudo sh /usr/share/ip-reporter/scripts/install.sh \
  --endpoint https://receiver.example.com/report
```

原生包安装程序到 `/usr/bin/ip-reporter`，脚本到 `/usr/share/ip-reporter/scripts/`。初始化脚本把运行用程序复制到 `/usr/local/bin/ip-reporter`，并将配置写入 `/etc/ip-reporter/config.json`。Linux 使用 `ip-reporter` 低权限账户和系统现有的 systemd / OpenRC。

有鉴权时加 `--token-file /root/ip-reporter-token`。令牌文件只提供令牌文本，随后可以自行删除；已安装的配置包含该令牌，权限限制为 root 和服务账户可读。升级原生包后再次运行 `install.sh`；默认保留设备标识和配置，需要重建配置时加 `--reconfigure`。

## TGZ / ZIP

Linux TGZ 解压后从 `<解压目录>/usr/share/ip-reporter/scripts/install.sh` 运行；macOS TGZ 从 `<解压目录>/share/ip-reporter/scripts/install.sh` 运行。脚本会定位同一包内的二进制：

```sh
sudo sh <脚本路径>/install.sh --endpoint https://receiver.example.com/report
```

Windows ZIP 解压后，在管理员 PowerShell 中运行：

```powershell
& .\share\ip-reporter\scripts\install.ps1 -Endpoint 'https://receiver.example.com/report'
```

Windows 使用 `%ProgramFiles%\IpReporter\ip-reporter.exe`、`%ProgramData%\IpReporter\config.json` 和 `IpReporter` 服务，账户为 `LocalService`。普通用户不能读取安装后的配置。源码构建时也可以显式传 `-BinaryPath` / `--binary`，见主文档。

## 卸载顺序

先用对应脚本停止并删除服务，再卸载原生包。默认保留配置及 Unix 服务账户；需要删除配置时加 `--purge-config` / `-PurgeConfig`。不会递归删除安装目录中的其他文件。

```sh
sudo sh /usr/share/ip-reporter/scripts/uninstall.sh
# Debian / Ubuntu
sudo apt remove ip-reporter
# Fedora / RHEL
sudo dnf remove ip-reporter
```

归档安装从原解压目录运行 `uninstall.sh` / `uninstall.ps1` 即可。macOS 服务名为 `org.ip-reporter.agent`，FreeBSD 服务名为 `ip_reporter`；两者完整操作见平台安装文档。macOS、OpenRC、FreeBSD 服务注册脚本需要在对应系统实测后再用于生产。
