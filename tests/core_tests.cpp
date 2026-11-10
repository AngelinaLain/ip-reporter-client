#include "ip_reporter/config.hpp"
#include "ip_reporter/network.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void rejects(const std::function<void()>& action, const std::string& message) {
    try {
        action();
        check(false, message);
    } catch (const std::exception&) {
    }
}

ip_reporter::Config valid_config() {
    ip_reporter::Config config;
    config.endpoint = "https://example.com/api/report";
    config.device_id = "unit-test-device";
    return config;
}

// 配置校验测试覆盖协议边界，避免无效配置进入常驻服务。
void test_config(const std::filesystem::path& directory) {
    using namespace ip_reporter;
    const auto base = valid_config();
    validate_config(base);
    check(base.poll_interval_seconds == 60, "default poll interval");
    check(base.heartbeat_interval_seconds == 300, "default heartbeat interval");
    check(base.request_timeout_seconds == 10, "default timeout");
    check(base.log_level == "error", "default error-only logging");
    check(!base.allow_http, "HTTPS by default");

    for (const auto& endpoint : {
             "", "https://", "ftp://example.com/report", "http://example.com/report",
             "https://user:password@example.com/report", "https://example.com/report#fragment",
             "https://example.com/\r\nInjected: yes", "https://example.com:invalid/report",
             "https://example.com:0/report", "https://example.com:65536/report",
             "https://[::1/report", "https://example.com report"}) {
        auto config = base;
        config.endpoint = endpoint;
        rejects([&] { validate_config(config); }, "reject unsafe endpoint: " + std::string(endpoint));
    }
    for (const auto& endpoint : {"https://example.com", "https://127.0.0.1:8443/api/report",
                                "https://[2001:db8::1]:8443/api/report?site=lab"}) {
        auto config = base;
        config.endpoint = endpoint;
        validate_config(config);
    }
    {
        auto config = base;
        config.endpoint = "http://127.0.0.1:8080/api/report";
        config.allow_http = true;
        validate_config(config);
    }
    for (const int interval : {-1, 0, 4, 86401}) {
        auto config = base;
        config.poll_interval_seconds = interval;
        rejects([&] { validate_config(config); }, "reject out-of-range poll interval");
    }
    for (const int interval : {-1, 1, 29, 86401}) {
        auto config = base;
        config.heartbeat_interval_seconds = interval;
        rejects([&] { validate_config(config); }, "reject out-of-range heartbeat interval");
    }
    for (const int timeout : {-1, 0, 121}) {
        auto config = base;
        config.request_timeout_seconds = timeout;
        rejects([&] { validate_config(config); }, "reject out-of-range timeout");
    }
    {
        auto config = base;
        config.heartbeat_interval_seconds = 0;
        validate_config(config);
        config.device_id.clear();
        rejects([&] { validate_config(config); }, "reject empty device id");
    }
    {
        auto config = base;
        config.bearer_token = "secret\r\nHeader: injected";
        rejects([&] { validate_config(config); }, "reject bearer header injection");
        config = base;
        config.log_level = "verbose-unknown";
        rejects([&] { validate_config(config); }, "reject unknown log level");
    }

    const auto path = directory / "roundtrip.json";
    auto config = base;
    config.device_id = "device-unit-test";
    config.bearer_token = "test-token-value";
    write_config(path, config);
    const auto loaded = load_config(path);
    check(config_to_json(loaded) == config_to_json(config), "configuration roundtrip");

    const auto write_json = [&](const nlohmann::json& value) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << value.dump();
    };
    auto bad = config_to_json(base);
    bad["unknown_option"] = true;
    write_json(bad);
    rejects([&] { (void)load_config(path); }, "reject unknown config key");
    bad = config_to_json(base);
    bad["poll_interval_seconds"] = "60";
    write_json(bad);
    rejects([&] { (void)load_config(path); }, "reject numeric string");
    bad["poll_interval_seconds"] = 60.5;
    write_json(bad);
    rejects([&] { (void)load_config(path); }, "reject fractional interval");
    bad = config_to_json(base);
    bad["allow_http"] = 1;
    write_json(bad);
    rejects([&] { (void)load_config(path); }, "reject non-boolean HTTP permission");
    bad = nlohmann::json::array();
    write_json(bad);
    rejects([&] { (void)load_config(path); }, "reject non-object config");
    rejects([&] { (void)load_config(directory / "missing.json"); }, "reject missing config");
}

// 地址测试明确区分设备实际可用地址和不应上报的特殊地址。
void test_addresses() {
    using ip_reporter::is_reportable_address;
    for (const auto& address : {"192.168.1.2", "10.0.0.1", "172.16.0.2", "8.8.8.8"}) {
        check(is_reportable_address(address, 4), "keep IPv4: " + std::string(address));
    }
    for (const auto& address : {"0.0.0.0", "127.0.0.1", "127.42.1.2", "169.254.1.2",
                                "224.0.0.1", "239.255.255.250", "255.255.255.255",
                                "999.1.2.3", "not-an-address", "::1"}) {
        check(!is_reportable_address(address, 4), "exclude IPv4: " + std::string(address));
    }
    for (const auto& address : {"2001:db8::1", "fd00::1234", "fc00::1"}) {
        check(is_reportable_address(address, 6), "keep IPv6: " + std::string(address));
    }
    for (const auto& address : {"::", "::1", "fe80::1", "febf::1", "ff02::1",
                                "ff0e::1234", "::ffff:192.168.1.2", "invalid", "1.2.3.4"}) {
        check(!is_reportable_address(address, 6), "exclude IPv6: " + std::string(address));
    }
    check(!is_reportable_address("192.168.1.2", 5), "reject unknown address family");
}

void test_payload() {
    using namespace ip_reporter;
    auto config = valid_config();
    config.bearer_token = "never-include-this-token";
    Snapshot snapshot;
    snapshot.hostname = "测试主机";
    snapshot.os = "linux";
    snapshot.interfaces = {
        {"eth0", {"192.168.1.2"}, {"2001:db8::1"}, "eth0", "02:00:00:00:00:01"},
        {"wlan0", {"10.0.0.2", "192.168.1.2"}, {"fd00::2", "2001:db8::1"}, "wlan0", "02:00:00:00:00:02"},
    };
    const auto payload = make_payload(config, snapshot, "address_change", 7, 42);
    check(payload.at("schema_version") == 1, "payload schema version");
    check(payload.at("device_id") == config.device_id, "payload device identity");
    check(payload.at("hostname") == snapshot.hostname, "payload UTF-8 hostname");
    check(payload.at("os") == "linux", "payload operating system");
    check(payload.at("event") == "address_change", "payload event");
    check(payload.at("status") == "alive", "payload liveness");
    check(payload.at("sequence") == 7, "payload sequence");
    check(payload.at("uptime_seconds") == 42, "payload monotonic uptime");
    check(payload.at("agent_version").is_string(), "payload agent version");
    check(payload.at("interfaces").size() == 2, "payload per-interface addresses");
    check(payload.at("interfaces")[0].at("id") == "eth0", "payload stable interface identity");
    check(payload.at("interfaces")[0].at("mac") == "02:00:00:00:00:01", "payload physical address");
    check(payload.at("ipv4").size() == 2, "payload IPv4 aggregate is deduplicated");
    check(payload.at("ipv6").size() == 2, "payload IPv6 aggregate is deduplicated");
    const auto timestamp = payload.at("timestamp").get<std::string>();
    check(timestamp.size() >= 20 && timestamp.back() == 'Z', "payload UTC timestamp");
    check(payload.dump().find(config.bearer_token) == std::string::npos, "payload excludes bearer token");
    auto reordered = snapshot;
    std::swap(reordered.interfaces[0], reordered.interfaces[1]);
    std::swap(reordered.interfaces[0].ipv4[0], reordered.interfaces[0].ipv4[1]);
    std::swap(reordered.interfaces[0].ipv6[0], reordered.interfaces[0].ipv6[1]);
    check(address_fingerprint(snapshot) == address_fingerprint(reordered),
          "address fingerprint ignores adapter enumeration order");
    reordered.interfaces[0].ipv4[0] = "10.0.0.3";
    check(address_fingerprint(snapshot) != address_fingerprint(reordered),
          "address fingerprint detects an address change");
    reordered = snapshot;
    reordered.interfaces[0].mac = "02:00:00:00:00:03";
    check(address_fingerprint(snapshot) != address_fingerprint(reordered),
          "address fingerprint detects a MAC change");
    reordered = snapshot;
    reordered.interfaces[0].id = "replacement-interface";
    check(address_fingerprint(snapshot) != address_fingerprint(reordered),
          "address fingerprint detects interface identity replacement");

    // 显示名称可重复，枚举顺序变化不应导致不断重复上报。
    auto duplicate_names = snapshot;
    duplicate_names.interfaces[0].name = duplicate_names.interfaces[1].name = "Ethernet";
    reordered = duplicate_names;
    std::swap(reordered.interfaces[0], reordered.interfaces[1]);
    check(address_fingerprint(duplicate_names) == address_fingerprint(reordered),
          "duplicate interface names still have a canonical fingerprint");
    Snapshot legacy;
    legacy.interfaces = {{"tun0", {"10.0.0.1"}, {}}};
    const auto no_mac = make_payload(config, legacy, "manual", 1, 0);
    check(no_mac.at("interfaces")[0].at("mac") == "", "unavailable MAC is an empty string");
}
}  // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("ip-reporter-tests-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(directory);
        test_config(directory);
        test_addresses();
        test_payload();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (failures == 0) {
        std::cout << "All core tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
