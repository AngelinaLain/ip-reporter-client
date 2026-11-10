#include "ip_reporter/agent.hpp"
#ifdef _WIN32
#include <windows.h>
#include <mutex>

namespace ip_reporter {
namespace {
constexpr wchar_t service_name[] = L"IpReporter";
SERVICE_STATUS_HANDLE status_handle = nullptr;
SERVICE_STATUS status{};
std::mutex status_mutex;
const Config* service_config = nullptr;
int service_result = 0;
void update_status(DWORD state, DWORD error = NO_ERROR) {
    std::lock_guard<std::mutex> guard(status_mutex);
    // 启动完成前、停止期间不接受额外停止命令。
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWin32ExitCode = error;
    status.dwWaitHint = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING ? 10000 : 0;
    status.dwCheckPoint = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING ? status.dwCheckPoint + 1 : 0;
    SetServiceStatus(status_handle, &status);
}
DWORD WINAPI control_handler(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        update_status(SERVICE_STOP_PENDING);
        request_stop();
    } else if (control == SERVICE_CONTROL_INTERROGATE) {
        std::lock_guard<std::mutex> guard(status_mutex);
        SetServiceStatus(status_handle, &status);
    }
    return NO_ERROR;
}
void WINAPI service_main(DWORD, wchar_t**) {
    status_handle = RegisterServiceCtrlHandlerExW(service_name, control_handler, nullptr);
    if (!status_handle) { service_result = 1; return; }
    update_status(SERVICE_START_PENDING);
    update_status(SERVICE_RUNNING);
    try { service_result = run_agent(*service_config); }
    catch (const std::exception&) { service_result = 1; }
    update_status(SERVICE_STOPPED, service_result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}
}
int run_service(const Config& config) {
    service_config = &config;
    SERVICE_TABLE_ENTRYW table[]{{const_cast<wchar_t*>(service_name), service_main}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) return 1;
    return service_result;
}
}
#else
namespace ip_reporter {
int run_service(const Config&) { throw std::runtime_error("--service is Windows-only; use the operating system service manager on POSIX"); }
}
#endif
