#include "ip_reporter/agent.hpp"
#include "ip_reporter/console.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
void help() {
    std::cout << "ip-reporter " IP_REPORTER_VERSION "\n"
        "Usage: ip-reporter [--config PATH] [--once | --print-payload | --check-config | --service]\n"
        "       ip-reporter --init --config PATH --endpoint URL [options]\n"
        "Init options: --token-file PATH --device-id ID --poll-seconds 60\n"
        "              --heartbeat-seconds 300 --timeout-seconds 10 --allow-http --force\n"
        "--init never connects. --force replaces an existing configuration.\n"
        "Without a mode flag the agent runs in the foreground (for a service manager).\n";
}
int integer_option(const std::string& value) {
    if (value.empty() || value.size() > 6 || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("invalid numeric option");
    return std::stoi(value);
}
int run(const std::vector<std::string>& args) {
    const std::set<std::string> switches{"--help", "--version", "--init", "--once", "--print-payload", "--check-config", "--service", "--allow-http", "--force"};
    const std::set<std::string> values{"--config", "--endpoint", "--token-file", "--device-id", "--poll-seconds", "--heartbeat-seconds", "--timeout-seconds"};
    std::map<std::string, std::string> options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (options.count(arg)) throw std::runtime_error("duplicate command line option");
        if (switches.count(arg)) options[arg] = "";
        else if (values.count(arg)) {
            if (++i >= args.size()) throw std::runtime_error("missing option value");
            options[arg] = args[i];
        } else throw std::runtime_error("unknown command line option; use --help");
    }
    if (options.count("--help")) { help(); return 0; }
    if (options.count("--version")) { std::cout << IP_REPORTER_VERSION << '\n'; return 0; }
    int modes = 0;
    for (const auto* mode : {"--init", "--once", "--print-payload", "--check-config", "--service"}) modes += options.count(mode) ? 1 : 0;
    if (modes > 1) throw std::runtime_error("select only one command mode");
    const auto path = options.count("--config") ? std::filesystem::u8path(options["--config"]) : ip_reporter::default_config_path();
    if (options.count("--init")) {
        if (!options.count("--endpoint")) throw std::runtime_error("--init requires --endpoint");
        if (std::filesystem::exists(path) && !options.count("--force")) throw std::runtime_error("configuration exists; use --force to replace it");
        ip_reporter::Config config;
        config.endpoint = options["--endpoint"];
        config.device_id = options.count("--device-id") ? options["--device-id"] : ip_reporter::make_device_id();
        config.allow_http = options.count("--allow-http") != 0;
        if (options.count("--poll-seconds")) config.poll_interval_seconds = integer_option(options["--poll-seconds"]);
        if (options.count("--heartbeat-seconds")) config.heartbeat_interval_seconds = integer_option(options["--heartbeat-seconds"]);
        if (options.count("--timeout-seconds")) config.request_timeout_seconds = integer_option(options["--timeout-seconds"]);
        if (options.count("--token-file")) {
            std::ifstream token(std::filesystem::u8path(options["--token-file"]), std::ios::binary);
            if (!token) throw std::runtime_error("cannot open token file");
            std::string data(4099, '\0');
            token.read(data.data(), static_cast<std::streamsize>(data.size()));
            data.resize(static_cast<std::size_t>(token.gcount()));
            // 允许常见文本文件结尾换行，拒绝 Token 内的空白与注入字符。
            if (!data.empty() && data.back() == '\n') data.pop_back();
            if (!data.empty() && data.back() == '\r') data.pop_back();
            if (data.empty()) throw std::runtime_error("token file is empty");
            config.bearer_token = data;
        }
        ip_reporter::validate_config(config);
        ip_reporter::write_config(path, config, options.count("--force") != 0);
        std::cout << "Configuration initialized.\n";
        return 0;
    }
    for (const auto& pair : options) {
        if (pair.first != "--config" && (values.count(pair.first) || pair.first == "--allow-http" || pair.first == "--force"))
            throw std::runtime_error("initialization options require --init");
    }
    const auto config = ip_reporter::load_config(path);
    if (options.count("--check-config")) { std::cout << "Configuration valid.\n"; return 0; }
    if (options.count("--print-payload")) {
        ip_reporter::print_stdout_utf8(ip_reporter::make_payload(config, ip_reporter::collect_snapshot(), "manual", 1, 0)
            .dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
        return 0;
    }
    ip_reporter::install_signal_handlers();
    if (options.count("--service")) return ip_reporter::run_service(config);
    return ip_reporter::run_agent(config, options.count("--once") != 0);
}
}
#ifdef _WIN32
// 保留原生 Unicode 命令行路径，适配中文用户名、目录和带空格的安装位置。
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 2;
        std::string value(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), size, nullptr, nullptr);
        value.pop_back();
        arguments.push_back(std::move(value));
    }
    try { return run(arguments); }
    catch (const std::exception& error) { std::cerr << "ip-reporter: " << error.what() << '\n'; return 2; }
}
#else
int main(int argc, char** argv) {
    try { return run(std::vector<std::string>(argv, argv + argc)); }
    catch (const std::exception& error) { std::cerr << "ip-reporter: " << error.what() << '\n'; return 2; }
}
#endif
