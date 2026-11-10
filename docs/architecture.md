# 架构与设计

## 模块

```text
配置与命令行
    ↓
地址采集 → 排序、去重 → 状态比较 → JSON 快照 → HTTPS POST
    ↑                                  ↓
操作系统网卡接口                成功等待 / 失败退避
    ↑                                  ↑
Windows SCM / systemd / OpenRC / launchd / rc.d
```

C++17 核心使用操作系统接口枚举网卡。Windows 网络传输使用原生 WinHTTP；POSIX 网络传输使用 libcurl，JSON 使用项目内固定版本的 nlohmann/json。不同系统编译各自的二进制，不通过运行时解释器模拟平台。

服务管理器负责开机启动、停止和异常重启，程序本身保持前台主进程模式供管理器托管。Windows 的 `--service` 入口与 SCM 控制处理器配合。手工调试直接执行程序；不在程序内部再次 fork 或创建隐蔽启动项。

## 资源与稳定性

- 默认每 60 秒枚举一次网卡，不持续忙循环。
- 地址不变时只按心跳间隔发送，不按轮询频率重复发送。
- Windows 网络请求使用统一截止时间；POSIX 设置 libcurl 传输和连接超时。失败退避，避免断网时持续建立连接。
- 单个发送流程串行执行，没有无上限工作线程或待发送队列。
- 不保存离线历史，不持续写本地报告文件；默认只输出错误，由服务管理器收集。
- 收到系统停止请求后退出循环；Windows 通过异步请求取消，POSIX 通过 libcurl 进度回调取消传输。

实际 CPU、内存和网络占用依操作系统、TLS 库、网卡数量和重试情况而变化。不能承诺绝对不会出现系统故障；需要在目标设备验证、监测服务日志和设置适合的上报频率。

POSIX 构建若使用 libcurl 的同步 DNS 解析器，在 `CURLOPT_NOSIGNAL` 下 DNS 查询期间可能不遵守传输超时，也无法立即执行进度回调取消。要求严格超时的环境应选择带 c-ares 或线程解析器的 libcurl；详情见[libcurl 文档](https://curl.se/libcurl/c/CURLOPT_NOSIGNAL.html)。

## 权限与透明运行

安装和卸载服务需要管理员 / root 权限；日常采集网卡地址和出站上报不需要高权限。安装器使用低权限运行账户，限制配置文件读取权限，避免 Token 被普通用户读取。Windows 使用 LocalService，Linux 使用专用服务账户，macOS 使用 nobody。

安装后无桌面交互、无浏览器、无托盘。服务名称、进程名称、配置文件和正常网络连接仍可被系统管理员查阅，不隐藏或伪装。卸载会移除服务和程序，默认保留配置供恢复；清除配置需显式参数。

## 通信与数据边界

只向管理员配置的接收地址发送主机名、设备标识和网卡地址。没有远程命令执行、文件下载执行、键盘捕获、屏幕采集、扫描或自动更新功能。Token 不放进 URL；不要在接收地址查询参数中嵌入秘密。

HTTPS 校验系统信任的服务证书，不提供忽略证书错误的开关。不跟随重定向，避免认证信息被转发到另一接收地址。明文 HTTP 必须显式开启；如需自建 CA，应把 CA 安装到运行环境信任库。

连接直接发往配置的接收地址，不读取系统代理或 `HTTP_PROXY` / `HTTPS_PROXY` 环境变量。本版本没有代理配置项；只允许经代理出站的网络需要单独适配或调整网络接入方案。

配置中的 Token 是明文秘密，依赖文件 ACL / Unix 权限保护。安装时的 Token 临时文件、备份、接收端日志和压缩包也需要妥善管理。心跳内容仍包含当前网络快照。

## 扩展方向

可在现有协议外增加接收端设备注册、每设备 Token、HTTPS 双向认证、观测指标和发布签名。若未来增加公网探测、代理配置或自动更新，应保持显式配置并补充相应测试；本版本没有实现这些功能。

相关官方资料：[systemd.service](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)、[WinHTTP 发送接口](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpsendrequest)、[libcurl 证书验证](https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYPEER.html)。
