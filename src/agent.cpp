#include "ip_reporter/agent.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <random>
#include <thread>

namespace ip_reporter {
namespace {
volatile std::sig_atomic_t signal_stop = 0;
std::atomic<bool> service_stop{false};
void signal_handler(int) { signal_stop = 1; }
using Clock = std::chrono::steady_clock;
}
void request_stop() noexcept { service_stop.store(true); }
bool stop_requested() noexcept { return signal_stop != 0 || service_stop.load(); }
void install_signal_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
}
void log_message(const Config& config, bool error, const std::string& message) {
    if (config.log_level == "silent" || (!error && config.log_level != "info")) return;
    std::cerr << "ip-reporter: " << (error ? "error: " : "info: ") << message << '\n';
}

int run_agent(const Config& config, bool once) {
    const auto started = Clock::now();
    std::uint64_t sequence = 0;
    auto uptime = [&] { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count()); };
    if (once) {
        const auto payload = make_payload(config, collect_snapshot(), "manual", ++sequence, uptime());
        const auto result = post_json(config, payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        if (!result.success) log_message(config, true, result.error + " (HTTP " + std::to_string(result.status) + ")");
        return result.success ? 0 : 1;
    }

    Snapshot snapshot;
    bool have_snapshot = false, reported = false;
    std::string fingerprint, last_reported;
    auto next_poll = started, next_attempt = started, last_success = started;
    int failures = 0;
    std::mt19937 random(std::random_device{}());
    // 单个调度循环，没有并发请求和持久化队列；只保留最新网卡快照。
    while (!stop_requested()) {
        auto now = Clock::now();
        if (now >= next_poll) {
            try {
                snapshot = collect_snapshot();
                fingerprint = address_fingerprint(snapshot);
                have_snapshot = true;
            } catch (const std::exception&) {
                have_snapshot = false;
                log_message(config, true, "cannot collect network addresses; retrying on next poll");
            }
            next_poll = now + std::chrono::seconds(config.poll_interval_seconds);
        }
        std::string event;
        if (!reported) event = "startup";
        else if (fingerprint != last_reported) event = "address_change";
        else if (config.heartbeat_interval_seconds > 0 && now - last_success >= std::chrono::seconds(config.heartbeat_interval_seconds)) event = "heartbeat";
        if (have_snapshot && !event.empty() && now >= next_attempt) {
            const auto payload = make_payload(config, snapshot, event, ++sequence, uptime());
            const auto result = post_json(config, payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            now = Clock::now();
            if (result.success) {
                last_reported = fingerprint;
                last_success = now;
                reported = true;
                failures = 0;
                next_attempt = now;
                log_message(config, false, "report accepted (" + event + ")");
            } else if (!stop_requested()) {
                // 指数退避限制为 5..300 秒，加少量抖动避免大量设备同时重连。
                const int base = std::min(300, 5 * (1 << std::min(failures, 6)));
                const int jitter = std::uniform_int_distribution<int>(0, std::max(1, base / 5))(random);
                const int delay = std::min(300, base + jitter);
                failures = std::min(failures + 1, 7);
                next_attempt = now + std::chrono::seconds(delay);
                log_message(config, true, result.error + " (HTTP " + std::to_string(result.status) + "); retry in " + std::to_string(delay) + "s");
            }
        }
        // 分片休眠让停止请求快速生效；休眠期间无轮询网络或磁盘写入。
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return 0;
}
}
