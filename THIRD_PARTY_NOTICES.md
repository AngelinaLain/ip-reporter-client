# 第三方组件

- **nlohmann/json 3.12.0**：MIT，源文件 `third_party/nlohmann/json.hpp`，许可证 `third_party/nlohmann/LICENSE.MIT`。来自上游固定版本：https://github.com/nlohmann/json/tree/v3.12.0 。头文件 SHA-256：`aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`。
- **libcurl**（仅 POSIX 构建）：使用系统提供的动态库，采用 curl 许可证，参见 https://curl.se/docs/copyright.html 。实际 TLS 后端及其许可证由目标系统的软件包决定。
- **Windows SDK / WinHTTP**：调用 Windows 自带 API，不附带第三方网络运行库。

分发程序时保留本文件、项目 LICENSE 和 nlohmann/json 的许可证。若静态链接其他依赖，请一并满足其许可证和分发要求。
