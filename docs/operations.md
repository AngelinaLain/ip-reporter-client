# 配置与运维

## 初始化

程序可直接以前台方式运行，也可由安装器注册系统服务。以下命令只写配置，不联网：

```sh
./ip-reporter --init --config ./config.json --endpoint https://report.example.com/api/report --token-file ./token.txt --device-id office-001 --poll-seconds 60 --heartbeat-seconds 300 --timeout-seconds 10
```

未指定 `--device-id` 时生成随机标识并写入配置；不要在每次重启时重新初始化。设备标识支持 1–128 个 ASCII 字母、数字、点、下划线和连字符。Token 为最多 4096 个非空白可打印 ASCII 字符；Token 文件应为 UTF-8 无 BOM，允许末尾一次 LF / CRLF。文件只在初始化时读取，运行时使用配置中的值。

`--init` 应用于新的配置路径，已有文件需要显式增加 `--force` 才会覆盖。配置所在目录需要事先存在，初始化命令不会自动建目录；安装脚本负责建目录，并先使用临时配置验证，再替换正式配置。手工操作正式配置前应备份，备份也需要限制读取权限。

## 配置字段

| 字段 | 默认值 | 约束 / 说明 |
| --- | --- | --- |
| `endpoint` | 必填 | 完整 HTTP(S) URL，最长 2048 字节；默认仅 HTTPS，不支持 URL 用户名密码 |
| `device_id` | 必填 | 持久设备标识；安装初始化时自动生成或显式指定 |
| `bearer_token` | 空 | 可选认证 Token，建议生产环境使用每设备独立 Token |
| `poll_interval_seconds` | `60` | 5–86400，检查地址变化的间隔 |
| `heartbeat_interval_seconds` | `300` | 30–86400；`0` 禁用定时心跳 |
| `request_timeout_seconds` | `10` | 1–120；网络操作的超时参数 |
| `allow_http` | `false` | 显式允许明文 HTTP，仅在受控环境启用 |
| `log_level` | `"error"` | `silent`、`error`、`info` |

配置必须为不超过 64 KiB 的 JSON 对象，使用正确的数据类型，不接受未知字段。示例见 `examples/config.example.json`，其中的域名和 Token 都需替换。JSON 不支持注释。

Windows 对网络请求使用统一截止时间和异步取消。POSIX 使用 libcurl 的传输 / 连接超时与进度回调取消；若目标 libcurl 使用同步 DNS 解析器，`CURLOPT_NOSIGNAL` 下的 DNS 阶段可能超过配置超时，停止请求也可能等待解析返回。需要严格 DNS 超时的部署应使用 c-ares 或线程解析器构建的 libcurl，参见[官方说明](https://curl.se/libcurl/c/CURLOPT_NOSIGNAL.html)。

默认路径：Windows `%ProgramData%\IpReporter\config.json`；其余系统 `/etc/ip-reporter/config.json`。所有命令都可以通过 `--config` 显式指定路径。

## 常用命令

```sh
ip-reporter --help
ip-reporter --version
ip-reporter --check-config --config /path/to/config.json
ip-reporter --print-payload --config /path/to/config.json
ip-reporter --once --config /path/to/config.json
ip-reporter --config /path/to/config.json
```

`--check-config` 不联网；`--print-payload` 采集并打印当前 JSON，不发送。`--once` 只尝试一次发送，以退出码表示成功或失败；常驻模式会重试。Windows 调试时可直接运行 `.exe`，作为系统服务运行时由安装器添加 `--service`。

常见退出码：`0` 成功或正常停止，`1` 为单次上报失败，`2` 为参数、配置或运行异常。服务管理器对异常退出的重启策略可能不同。

配置修改后需要重启服务才能生效；程序不监视配置文件。可先用 `--check-config` 验证候选文件，再替换和重启。权限不足时请在管理员终端操作，不要为了排错把配置改成全员可读。

## 服务管理

### Windows SCM

在管理员 PowerShell 中：

```powershell
Get-Service IpReporter
Start-Service IpReporter
Stop-Service IpReporter
Restart-Service IpReporter
sc.exe qc IpReporter
```

服务使用延迟自动启动，异常退出由 SCM 按安装时设置重启。默认不弹窗，也不创建本地持续增长的日志文件。诊断网络问题可在停止服务后，以管理员身份用同一配置运行 `--once` 或前台模式；避免同时启动多个实例导致重复报告。

### Linux systemd

```sh
sudo systemctl status ip-reporter
sudo systemctl restart ip-reporter
sudo systemctl stop ip-reporter
sudo journalctl -u ip-reporter -n 100 --no-pager
```

服务文件位于 `/etc/systemd/system/ip-reporter.service`。安装器设置低权限账户、较低调度优先级及系统目录保护。日志由 journald 管理，保留策略由系统管理员设置。

### Linux OpenRC

```sh
sudo rc-service ip-reporter status
sudo rc-service ip-reporter restart
sudo rc-service ip-reporter stop
rc-update show default
```

脚本位于 `/etc/init.d/ip-reporter`，使用 OpenRC supervise-daemon 管理进程。

### macOS launchd

```sh
sudo launchctl print system/org.ip-reporter.agent
sudo launchctl kickstart -k system/org.ip-reporter.agent
sudo launchctl bootout system/org.ip-reporter.agent
sudo launchctl bootstrap system /Library/LaunchDaemons/org.ip-reporter.agent.plist
```

`bootout` 停止并卸载当前任务，`bootstrap` 重新载入。标准位置及管理机制参见 [Apple launchd 说明](https://support.apple.com/en-asia/guide/terminal/apdc6c1077b-5d5d-4d35-9c19-60f2397b2369/mac)。

### FreeBSD rc.d

```sh
sudo service ip_reporter status
sudo service ip_reporter restart
sudo service ip_reporter stop
sysrc ip_reporter_enable
```

脚本位于 `/usr/local/etc/rc.d/ip_reporter`。启动配置参见 [FreeBSD 服务文档](https://docs.freebsd.org/en/books/handbook/config/)。

## 升级与重新配置

保存新版本二进制后再次执行安装器并指定 `--binary` / `-BinaryPath`。已有配置默认保留，`device_id` 和 Token 不变。修改初始化参数需显式增加 `--reconfigure` / `-Reconfigure`，否则传入的新接收地址不会覆盖现有配置。

重新配置会完整重建配置，不会与旧字段合并：省略 `device-id` 会生成新的设备标识，省略 `token-file` 会清空 Token，其余未指定项回到默认值。要保持接收端设备记录和鉴权，请显式传入原标识及 Token 文件；只调整单个字段也可直接以管理员身份编辑配置、校验后重启。停止服务、替换程序或配置、再启动会产生短暂上报间隔。

安装器会在停止原服务之前验证候选配置，但不是完整事务安装系统；复制、权限或服务管理器报错后，应检查实际服务状态并恢复之前的程序与配置。项目不提供自动下载更新。

## 卸载

Windows 管理员 PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1
# 连配置一起清除
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1 -PurgeConfig
```

Linux / macOS / FreeBSD：

```sh
sudo sh scripts/uninstall.sh
# 连配置一起清除
sudo sh scripts/uninstall.sh --purge-config
```

默认删除服务和二进制，保留配置及服务账户，便于恢复并保持设备标识；清除配置不会删除任意用户目录或接收端历史数据。通过 RPM / DEB 安装的文件还应使用系统包管理器移除，见打包说明。

## 排错

### PowerShell 命令与运行模式

即使已经进入 `.exe` 所在目录，PowerShell 仍需使用 `.\ip-reporter.exe`；直接输入 `ip-reporter.exe` 通常不会搜索当前目录。若从项目根目录运行，则使用 `.\build\Release\ip-reporter.exe` 或实际构建路径。

接收地址是普通字符串，例如 `--endpoint "http://127.0.0.1:8080/api/report"`。本地接收示例使用明文 HTTP，初始化时须带 `--allow-http`；真实 HTTPS 地址不需要该参数。`--endpoint` 等初始化参数只与 `--init` 配合使用。

| 命令模式 | 执行结果 |
| --- | --- |
| `--init` | 写入配置，然后退出；不发送 |
| `--check-config` | 校验配置，然后退出；不发送 |
| `--print-payload` | 打印当前 JSON，然后退出；不发送 |
| `--once` | 发送一次，然后退出；紧接着检查 `$LASTEXITCODE` |
| 只有 `--config` | 持续检查、上报和发送心跳，直到按 `Ctrl+C` 或收到停止请求 |
| 安装为系统服务 | 由系统在后台启动和管理，不需要保留终端 |

默认 `log_level` 为 `error`，单次发送成功时不会打印成功消息。`$LASTEXITCODE` 为 `0`，并且接收端出现报告，即表示本次发送成功。需要持续运行时去掉 `--once` / `--print-payload`，不要通过反复执行初始化命令维持上报。

### 中文显示、时间和地址

当前版本在 Windows 原生控制台使用 Unicode 输出中文网卡名；重定向输出保留 UTF-8 字节。若旧二进制显示乱码，先用当前编译产物重试；读取接收器写出的 UTF-8 JSONL 文件时可显式指定编码：

```powershell
Get-Content -LiteralPath .\reports.jsonl -Encoding UTF8 -Tail 1
```

PowerShell 自身的管道与文件重定向编码随版本而不同，不能用终端中已有的乱码判断网卡名称已损坏；应同时检查当前程序直接输出和接收器保存的 JSONL。

`timestamp` 末尾的 `Z` 表示 UTC。例如 `2026-11-10T08:00:00Z` 对应北京时间 `2026-11-10 16:00:00`。这与北京时间相差八小时是预期的协议格式；接收端另有自己的 `received_at`。

`ipv4` / `ipv6` 来自本机网卡，因此多网卡、VPN、虚拟网卡和私网地址可能同时出现。回环和链路本地地址已过滤；只配置了 IPv6 链路本地地址的设备，其 IPv6 数组可能为空。接收器的 `peer_ip` 是此次连接的对端地址：本机回环测试会是 `127.0.0.1`，远程连接才可能观察到 NAT 出口，详见[协议说明](protocol.md)。

| 现象 | 检查 |
| --- | --- |
| PowerShell 找不到当前目录程序 | 使用 `.\ip-reporter.exe` 或完整的可执行文件路径 |
| HTTP 初始化被拒绝 | 本地 HTTP 测试在 `--init` 时加 `--allow-http` |
| `--once` 后无输出 | 默认成功静默；立即查看 `$LASTEXITCODE` 和接收端报告 |
| 打印 JSON 后程序结束 | `--print-payload` 只打印；持续运行使用 `--config` |
| 服务启动后退出 | 用 `--check-config` 检查 JSON；确认服务账户有配置读取权，系统库可用 |
| HTTPS 失败 | 检查证书有效期、系统时间、主机名 / IP SAN、CA 信任和目标端口 |
| 收到 401 / 403 | 核对接收端 Token，与服务配置是否一致；修改后重启服务 |
| 没有 IPv6 | 检查网卡是否启用、是否只有链路本地 IPv6；本程序不自动分配 IPv6 |
| 收到的是内网 IPv4 | 这是本机网卡地址；公网出口需接收端记录连接对端 |
| 很久没有报告 | 检查是否禁用了心跳、系统是否休眠、网络是否失败退避、接收端是否可达 |
| 局域网接收端不可达 | 确认服务监听地址、设备路由、出站防火墙；本程序不开放本机入站端口 |
| 修改配置无效 | 校验候选文件后重启；重新运行安装器改参数时要带重新配置开关 |

主机处于休眠、关机或完全断网时无法发送保活。程序会在停止时取消进行中的网络操作；POSIX 同步 DNS 解析的例外见上文。无需通过高频重启弥补网络不可达。
