#pragma once
#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>

namespace ip_reporter {
struct Config {
    std::string endpoint;
    std::string device_id;
    std::string bearer_token;
    int poll_interval_seconds = 60;
    int heartbeat_interval_seconds = 300;
    int request_timeout_seconds = 10;
    bool allow_http = false;
    std::string log_level = "error";
};
struct Endpoint {
    bool secure;
    std::string host;
    unsigned short port;
    std::string target;
};
Endpoint parse_endpoint(const std::string& url, bool allow_http);
void validate_config(const Config& config);
Config load_config(const std::filesystem::path& path);
void write_config(const std::filesystem::path& path, const Config& config, bool overwrite = true);
nlohmann::json config_to_json(const Config& config);
std::string make_device_id();
std::filesystem::path default_config_path();
}
