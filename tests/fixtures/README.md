`localhost-test.crt` 与 `localhost-test.key` 是公开的自动化测试夹具，仅用于本机 HTTPS 接收器。
该证书自签名，包含 localhost、127.0.0.1 和 ::1，用于确认客户端拒绝不受信任的服务器，且不会在验证失败时发送 Token。
不得将这些夹具用于实际服务或导入系统信任库。
