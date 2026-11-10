#include "ip_reporter/agent.hpp"
#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace ip_reporter {
#ifdef _WIN32
namespace {
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) throw std::runtime_error("invalid UTF-8 HTTP parameter");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
struct InternetHandle {
    HINTERNET value;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};
// 使用异步通知配合统一截止时间；退出服务时无需等待漫长的 DNS/网络超时。
struct RequestState {
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<DWORD> error{0};
    ~RequestState() { if (ready) CloseHandle(ready); if (closed) CloseHandle(closed); }
};
void CALLBACK request_callback(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD) {
    auto* state = reinterpret_cast<RequestState*>(context);
    if (!state) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { SetEvent(state->closed); return; }
    if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        state->error.store(static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError);
        SetEvent(state->ready);
    } else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE || status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) {
        SetEvent(state->ready);
    }
}
struct AsyncRequest {
    HINTERNET value;
    RequestState& state;
    ~AsyncRequest() {
        if (value) {
            WinHttpCloseHandle(value);
            // HANDLE_CLOSING 是最后一个回调，之后才能释放回调上下文。
            WaitForSingleObject(state.closed, INFINITE);
        }
    }
};
bool await_request(RequestState& state, std::chrono::steady_clock::time_point deadline) {
    while (!stop_requested()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return false;
        const auto wait = WaitForSingleObject(state.ready, static_cast<DWORD>(std::min<long long>(remaining, 250)));
        if (wait == WAIT_OBJECT_0) return state.error.load() == 0;
        if (wait == WAIT_FAILED) return false;
    }
    return false;
}
}

HttpResult post_json(const Config& config, const std::string& body) {
    const auto endpoint = parse_endpoint(config.endpoint, config.allow_http);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(config.request_timeout_seconds);
    InternetHandle session{WinHttpOpen(L"ip-reporter/" IP_REPORTER_VERSION, WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
    if (!session.value) return {false, 0, "WinHTTP session initialization failed"};
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
            return {false, 0, "cannot require TLS 1.2 or later"};
    }
    const auto ms = config.request_timeout_seconds * 1000;
    if (!WinHttpSetTimeouts(session.value, ms, ms, ms, ms)) return {false, 0, "cannot set HTTP timeouts"};
    // WinHTTP 的连接目标要求 IPv6 字面量保留方括号。
    const auto host = wide(endpoint.host.find(':') == std::string::npos ? endpoint.host : "[" + endpoint.host + "]");
    InternetHandle connection{WinHttpConnect(session.value, host.c_str(), endpoint.port, 0)};
    if (!connection.value) return {false, 0, "cannot create HTTP connection"};
    const auto target = wide(endpoint.target);
    auto raw = WinHttpOpenRequest(connection.value, L"POST", target.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, endpoint.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!raw) return {false, 0, "cannot create HTTP request"};
    RequestState state;
    if (!state.ready || !state.closed) { WinHttpCloseHandle(raw); return {false, 0, "cannot allocate HTTP event"}; }
    const DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&state);
    if (!WinHttpSetOption(raw, WINHTTP_OPTION_CONTEXT_VALUE, const_cast<DWORD_PTR*>(&context), sizeof(context)) ||
        WinHttpSetStatusCallback(raw, request_callback, WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
            WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        WinHttpCloseHandle(raw);
        return {false, 0, "cannot configure HTTP callback"};
    }
    AsyncRequest request{raw, state};
    DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    if (!WinHttpSetOption(raw, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable))) return {false, 0, "cannot disable redirects"};
    std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\nAccept: application/json\r\n";
    if (!config.bearer_token.empty()) headers += L"Authorization: Bearer " + wide(config.bearer_token) + L"\r\n";
    if (!WinHttpSendRequest(raw, headers.c_str(), static_cast<DWORD>(headers.size()), const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), context) || !await_request(state, deadline))
        return {false, 0, "HTTP send failed, cancelled or timed out"};
    if (!WinHttpReceiveResponse(raw, nullptr) || !await_request(state, deadline))
        return {false, 0, "HTTP response failed, cancelled or timed out"};
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(raw, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &status, &size, WINHTTP_NO_HEADER_INDEX)) return {false, 0, "cannot read HTTP response status"};
    return {status >= 200 && status < 300, static_cast<long>(status), status >= 200 && status < 300 ? "" : "server returned non-2xx status"};
}
#else
namespace {
struct CurlRuntime {
    CurlRuntime() { if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("curl initialization failed"); }
    ~CurlRuntime() { curl_global_cleanup(); }
};
std::size_t discard_response(char*, std::size_t size, std::size_t count, void* context) {
    auto& received = *static_cast<std::size_t*>(context);
    const auto bytes = size * count;
    if (bytes > 65536 - received) return 0;
    received += bytes;
    return bytes;
}
int transfer_progress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return stop_requested() ? 1 : 0; }
}
HttpResult post_json(const Config& config, const std::string& body) {
    static CurlRuntime runtime;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return {false, 0, "cannot create HTTP session"};
    curl_slist* raw_headers = nullptr;
    for (const auto& header : {std::string("Content-Type: application/json; charset=utf-8"), std::string("Accept: application/json"), std::string("Expect:")})
        raw_headers = curl_slist_append(raw_headers, header.c_str());
    if (!config.bearer_token.empty()) raw_headers = curl_slist_append(raw_headers, ("Authorization: Bearer " + config.bearer_token).c_str());
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
    std::size_t received = 0;
    curl_easy_setopt(curl.get(), CURLOPT_URL, config.endpoint.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "ip-reporter/" IP_REPORTER_VERSION);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(config.request_timeout_seconds));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, static_cast<long>(config.request_timeout_seconds));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, discard_response);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &received);
    const auto result = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (result != CURLE_OK) return {false, status, "HTTP transfer failed (curl code " + std::to_string(static_cast<int>(result)) + ")"};
    return {status >= 200 && status < 300, status, status >= 200 && status < 300 ? "" : "server returned non-2xx status"};
}
#endif
}
