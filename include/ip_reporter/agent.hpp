#pragma once
#include "network.hpp"
#include <atomic>
#include <string>

namespace ip_reporter {
// 仅用于退出协作；信号处理使用 sig_atomic_t，避免在处理器中执行非安全操作。
void request_stop() noexcept;
bool stop_requested() noexcept;
void install_signal_handlers();
struct HttpResult { bool success; long status; std::string error; };
HttpResult post_json(const Config&, const std::string& body);
int run_agent(const Config&, bool once = false);
int run_service(const Config&);
void log_message(const Config&, bool error, const std::string& message);
}
