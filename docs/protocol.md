# 上报协议

## 传输

向配置的 `endpoint` 发送 `POST`，请求头包含 `Content-Type: application/json`。配置了 `bearer_token` 时添加 `Authorization: Bearer <token>`。正文为 UTF-8 标准 JSON。

HTTPS 为默认要求：校验证书链和主机名，不接受自签名或名称不匹配的证书，不自动跟随 3xx 重定向。自建 CA 应安装到系统 / libcurl 使用的信任库。生产接收地址建议使用稳定域名；直接使用 IP 时证书也必须覆盖该 IP。HTTP 需显式配置 `allow_http: true`。

程序直接连接目标，不使用系统或环境变量中的 HTTP(S) 代理。网络超时与 POSIX 同步 DNS 的限制见[运维说明](operations.md)。

仅 HTTP 2xx 表示接收成功；3xx、4xx、5xx 和网络错误均按失败处理。接收端应先验证、保存报告，再返回成功。服务不依赖响应正文，也不从响应下载或执行任何内容。

## 字段

| 字段 | JSON 类型 | 含义 |
| --- | --- | --- |
| `schema_version` | integer | 当前为 `1`，协议结构版本 |
| `agent_version` | string | 程序版本，如 `1.1.0` |
| `device_id` | string | 安装时生成或指定的稳定设备标识 |
| `timestamp` | string | 客户端 UTC 时间，RFC 3339，例 `2026-11-10T08:00:00Z` |
| `sequence` | integer | 此进程中的发送序号；重启后重新开始，不是全局唯一键 |
| `event` | string | `startup`、`address_change`、`heartbeat`、`manual` |
| `hostname` | string | 系统主机名 |
| `os` | string | 操作系统标识，如 `windows`、`linux`、`macos`、`freebsd` |
| `ipv4` | array of string | 去重后的本机 IPv4 地址 |
| `ipv6` | array of string | 去重后的本机 IPv6 地址 |
| `interfaces` | array of object | 每个有效网卡的 `id`、`name`、`mac`、`ipv4`、`ipv6` |
| `uptime_seconds` | integer | 当前上报进程已运行秒数，不是系统开机时长 |
| `status` | string | 当前固定为 `alive` |

IPv6 使用无方括号的地址文本；URL 中的 IPv6 字面量才使用方括号。数组元素排序后参与变化检测，避免系统枚举顺序变化产生误报。网卡变更和空地址集合也可形成新状态。

程序采集活动网卡，默认排除回环、IPv4 链路本地和 IPv6 链路本地地址；不会扫描局域网、读取浏览记录或查询第三方 IP 服务。全局单播 IPv6、IPv6 ULA、私网 IPv4、VPN / 虚拟网卡可能出现在报告中。

### 网卡标识与 MAC（客户端 1.1.0 起）

```json
{
  "id": "{B4EA18C5-89B6-4721-B74B-17FE32C26F15}",
  "name": "以太网",
  "mac": "02:00:00:00:00:01",
  "ipv4": ["192.168.1.10"],
  "ipv6": ["2001:db8::10"]
}
```

`id` 是 DDNS 规则选择网卡时使用的键：Windows 使用系统的适配器 GUID，因此修改显示名称后仍能关联同一网卡；Linux、macOS 和 FreeBSD 使用接口名，如 `eth0`、`en0`。在 POSIX 系统重命名接口、重建虚拟网卡或替换硬件后，应核对并重新绑定受影响规则。`name` 用于页面显示，可能包含中文，也可能与其他网卡同名。

`mac` 为系统报告的物理地址，使用小写十六进制和冒号分隔；通常为 6 字节。支持系统接口提供的 6～20 字节地址，实际可读取长度受平台 API 限制。隧道、VPN 或没有物理地址的接口使用空字符串，不伪造 MAC，也不会主动探测网络中的其他设备。隐私地址随机化、虚拟机配置或系统设置都可能改变 MAC，因此不要仅以 MAC 作为永久机器标识。

这是对 `schema_version: 1` 的兼容性扩展。1.0.x 客户端仍可上报原格式，接收端应把缺少的 `mac` 视为未知，并以网卡 `name` 作为旧客户端的选择依据。升级至 1.1.0 后的报告包含 `id` 和 `mac`；身份、物理地址或 IP 改变会触发下一轮 `address_change`。已有旧规则在改用稳定 `id` 前应重新核对绑定。

## 公网地址与 NAT

报告中的 `ipv4` / `ipv6` 描述本机配置。NAT、代理、多出口或 VPN 可能使接收端观察到另一地址。接收端可以另记 `peer_ip`，但它表示 TCP 对端，经过反向代理后可能是代理地址。

需要真实客户端出口时，应由受信任的反向代理写入并在后端校验转发信息。不要无条件相信客户端提供的 `X-Forwarded-For`。同时测量 IPv4 和 IPv6 公网出口需要可分别强制地址族的两个探测通道；本版本没有这一额外探测功能。

## 发送时机、重试与在线判断

服务启动立即尝试发送 `startup`；轮询发现地址变化发送 `address_change`；地址不变时按心跳间隔发送 `heartbeat`。`--once` 使用 `manual`。心跳间隔为 `0` 可禁用定时心跳。

失败采用带抖动的指数退避，基准从 5 秒增长至最多 300 秒；连接恢复后重新上报当前状态。服务不会无限保存历史报告，也不保证离线期间每一个中间地址都能补发。接收端应按“状态快照”设计，不应将它当作审计事件队列。

请求可能已被接收，但客户端未读到响应，因此接收端应容忍重复快照。不要只用 `device_id + sequence` 作为跨进程去重键。需要强去重时可结合接收时间、设备标识和报告内容，并为未来协议加入会话 ID 留出扩展空间。

在线状态应依据接收端自己的 `received_at`。例如心跳为 300 秒，可在超过 900 秒且没有报告后判为离线；阈值应覆盖网络抖动、超时、重试和系统休眠。客户端 `timestamp` 受设备时钟影响，不宜单独用于离线判断。禁用心跳后，不能仅凭“长期没有地址变化报告”判断服务停止。

## 接收示例

```sh
python tools/receiver.py --listen 127.0.0.1 --port 8080 --token-file ./receiver.token --output ./reports.jsonl
```

示例接受 `/api/report`，限制正文 64 KiB，验证核心字段、地址族和 Token，成功返回 `204`；每行输出：

```json
{"received_at":"2026-11-10T08:00:01Z","peer_ip":"127.0.0.1","report":{"schema_version":1}}
```

上例中的 `report` 为缩略展示，真实输出含完整报告。默认仅监听 `127.0.0.1`；`--listen ::1` 可监听 IPv6 回环。测试示例通过标准库 HTTPServer 提供明文 HTTP，应放在 HTTPS 反向代理之后用于试验；生产环境还需补充并发、限流、设备鉴权管理、存储、日志轮转和备份。JSONL 文件由接收端负责维护，设备端不会创建它。
