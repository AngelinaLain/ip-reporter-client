#include "ip_reporter/config.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sddl.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace ip_reporter {
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool ascii_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
}

// 只允许明确的 HTTP(S) 目标，防止凭据、控制字符和重定向造成意外泄露。
Endpoint parse_endpoint(const std::string& url, bool allow_http) {
    require(!url.empty() && url.size() <= 2048, "endpoint length must be 1..2048");
    require(std::all_of(url.begin(), url.end(), [](unsigned char c) { return c > 32 && c < 127; }),
            "endpoint must contain printable ASCII without spaces");
    const bool secure = url.rfind("https://", 0) == 0;
    require(secure || (allow_http && url.rfind("http://", 0) == 0), "HTTPS required; use allow_http only for trusted test networks");
    require(url.find('#') == std::string::npos && url.find('\\') == std::string::npos,
            "endpoint fragments and backslashes are not allowed");
    const auto begin = secure ? 8u : 7u;
    const auto end = url.find_first_of("/?", begin);
    const auto authority = url.substr(begin, end == std::string::npos ? end : end - begin);
    require(!authority.empty() && authority.find('@') == std::string::npos, "endpoint host missing or userinfo present");
    std::string host, port;
    if (authority.front() == '[') {
        const auto bracket = authority.find(']');
        require(bracket != std::string::npos, "invalid bracketed IPv6 host");
        host = authority.substr(1, bracket - 1);
        unsigned char bytes[16]{};
        require(inet_pton(AF_INET6, host.c_str(), bytes) == 1, "invalid IPv6 host");
        if (bracket + 1 < authority.size()) {
            require(authority[bracket + 1] == ':', "invalid endpoint port");
            port = authority.substr(bracket + 2);
            require(!port.empty(), "empty endpoint port");
        }
    } else {
        auto colon = authority.find(':');
        host = authority.substr(0, colon);
        require(!host.empty() && host.size() <= 253 && host.front() != '.' && host.back() != '.', "invalid endpoint host");
        require(std::all_of(host.begin(), host.end(), [](unsigned char c) { return ascii_alnum(c) || c == '-' || c == '.'; }), "invalid endpoint host");
        std::istringstream labels(host);
        std::string label;
        while (std::getline(labels, label, '.')) {
            require(!label.empty() && label.size() <= 63 && label.front() != '-' && label.back() != '-', "invalid DNS label");
        }
        if (colon != std::string::npos) { port = authority.substr(colon + 1); require(!port.empty(), "empty endpoint port"); }
        if (std::all_of(host.begin(), host.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || c == '.'; })) {
            unsigned char bytes[4]{};
            require(inet_pton(AF_INET, host.c_str(), bytes) == 1, "invalid IPv4 host");
        }
    }
    unsigned int number = secure ? 443u : 80u;
    if (!port.empty()) {
        require(port.size() <= 5 && std::all_of(port.begin(), port.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }), "invalid endpoint port");
        number = static_cast<unsigned int>(std::stoul(port));
        require(number > 0 && number <= 65535, "endpoint port out of range");
    }
    std::string target = end == std::string::npos ? "/" : url.substr(end);
    if (target.front() == '?') target.insert(target.begin(), '/');
    return {secure, host, static_cast<unsigned short>(number), target};
}

void validate_config(const Config& c) {
    parse_endpoint(c.endpoint, c.allow_http);
    require(!c.device_id.empty() && c.device_id.size() <= 128 &&
        std::all_of(c.device_id.begin(), c.device_id.end(), [](unsigned char ch) { return ascii_alnum(ch) || ch == '.' || ch == '_' || ch == '-'; }),
        "device_id must use 1..128 ASCII letters, digits, dot, underscore or dash");
    require(c.bearer_token.size() <= 4096 && std::all_of(c.bearer_token.begin(), c.bearer_token.end(),
        [](unsigned char ch) { return ch > 32 && ch < 127; }), "bearer_token must be printable ASCII without whitespace (max 4096)");
    require(c.poll_interval_seconds >= 5 && c.poll_interval_seconds <= 86400, "poll_interval_seconds must be 5..86400");
    require(c.heartbeat_interval_seconds == 0 || (c.heartbeat_interval_seconds >= 30 && c.heartbeat_interval_seconds <= 86400), "heartbeat_interval_seconds must be 0 or 30..86400");
    require(c.request_timeout_seconds >= 1 && c.request_timeout_seconds <= 120, "request_timeout_seconds must be 1..120");
    require(c.log_level == "silent" || c.log_level == "error" || c.log_level == "info", "log_level must be silent, error or info");
}

nlohmann::json config_to_json(const Config& c) {
    return {{"endpoint", c.endpoint}, {"device_id", c.device_id}, {"bearer_token", c.bearer_token},
        {"poll_interval_seconds", c.poll_interval_seconds}, {"heartbeat_interval_seconds", c.heartbeat_interval_seconds},
        {"request_timeout_seconds", c.request_timeout_seconds}, {"allow_http", c.allow_http}, {"log_level", c.log_level}};
}

Config load_config(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "cannot open configuration file");
    std::string content(65537, '\0');
    stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(stream.gcount()));
    require(content.size() <= 65536, "configuration exceeds 64 KiB");
    Config c;
    try {
        const auto j = nlohmann::json::parse(content);
        require(j.is_object(), "configuration must be a JSON object");
        const std::set<std::string> allowed{"endpoint", "device_id", "bearer_token", "poll_interval_seconds",
            "heartbeat_interval_seconds", "request_timeout_seconds", "allow_http", "log_level"};
        for (auto it = j.begin(); it != j.end(); ++it) require(allowed.count(it.key()) != 0, "unknown configuration field");
        for (const auto* name : {"endpoint", "device_id", "bearer_token", "log_level"})
            if (j.contains(name)) require(j[name].is_string(), "string configuration field has wrong type");
        for (const auto* name : {"poll_interval_seconds", "heartbeat_interval_seconds", "request_timeout_seconds"})
            if (j.contains(name)) require(j[name].is_number_integer() && j[name] >= 0 && j[name] <= 86400, "integer configuration field has wrong type or range");
        if (j.contains("allow_http")) require(j["allow_http"].is_boolean(), "allow_http must be boolean");
        c.endpoint = j.at("endpoint").get<std::string>();
        c.device_id = j.at("device_id").get<std::string>();
        c.bearer_token = j.value("bearer_token", c.bearer_token);
        c.poll_interval_seconds = j.value("poll_interval_seconds", c.poll_interval_seconds);
        c.heartbeat_interval_seconds = j.value("heartbeat_interval_seconds", c.heartbeat_interval_seconds);
        c.request_timeout_seconds = j.value("request_timeout_seconds", c.request_timeout_seconds);
        c.allow_http = j.value("allow_http", c.allow_http);
        c.log_level = j.value("log_level", c.log_level);
    } catch (const nlohmann::json::exception&) {
        // 不把原始 JSON/解析上下文写入日志，避免泄漏 Token。
        throw std::runtime_error("invalid JSON configuration or missing required fields");
    }
    validate_config(c);
    return c;
}

std::string make_device_id() {
    std::random_device random;
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) result << std::setw(8) << static_cast<std::uint32_t>(random());
    return result.str();
}

// 同目录临时文件 + 原子替换，防止异常退出留下半份配置；临时文件默认仅管理员/所有者可读。
void write_config(const std::filesystem::path& path, const Config& c, bool overwrite) {
    validate_config(c);
    const auto temporary = std::filesystem::path(path.native() + std::filesystem::path("." + make_device_id() + ".tmp").native());
    const auto content = config_to_json(c).dump(2) + "\n";
#ifdef _WIN32
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr) != 0, "cannot create configuration security descriptor");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(descriptor);
    require(file != INVALID_HANDLE_VALUE, "cannot create temporary configuration");
    DWORD written = 0;
    const bool ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
                    written == content.size() && FlushFileBuffers(file);
    CloseHandle(file);
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), flags)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("cannot commit configuration file");
    }
#else
    const int file = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    require(file >= 0, "cannot create temporary configuration");
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto written = write(file, content.data() + offset, content.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        offset += static_cast<std::size_t>(written);
    }
    const bool ok = offset == content.size() && fsync(file) == 0;
    const int closed = close(file);
    const bool committed = ok && closed == 0 && (overwrite ? rename(temporary.c_str(), path.c_str()) == 0 : link(temporary.c_str(), path.c_str()) == 0);
    if (!committed) {
        unlink(temporary.c_str());
        throw std::runtime_error("cannot commit configuration file");
    }
    if (!overwrite) unlink(temporary.c_str());
#endif
}

std::filesystem::path default_config_path() {
#ifdef _WIN32
    wchar_t directory[32768]{};
    auto count = GetEnvironmentVariableW(L"ProgramData", directory, 32768);
    if (count > 0 && count < 32768) return std::filesystem::path(directory) / "IpReporter" / "config.json";
    return L"C:\\ProgramData\\IpReporter\\config.json";
#else
    return "/etc/ip-reporter/config.json";
#endif
}
}
