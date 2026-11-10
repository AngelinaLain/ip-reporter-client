#include "ip_reporter/console.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

namespace ip_reporter {
void print_stdout_utf8(std::string_view text) {
    if (text.empty()) return;
#ifdef _WIN32
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && output != nullptr && GetConsoleMode(output, &mode)) {
        // std::cout 的 UTF-8 字节在 CP936 等终端会被误解码；W API 绕过代码页，
        // 无需修改调用者的控制台设置。重定向时则继续输出原始 UTF-8 字节。
        if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
            throw std::runtime_error("console output exceeds supported length");
        const int byte_count = static_cast<int>(text.size());
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), byte_count, nullptr, 0);
        if (count <= 0) throw std::runtime_error("invalid UTF-8 console output");
        std::wstring wide(static_cast<std::size_t>(count), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), byte_count, wide.data(), count))
            throw std::runtime_error("cannot convert console output");
        std::cout.flush();
        std::size_t offset = 0;
        while (offset < wide.size()) {
            auto chunk = (std::min)(std::size_t{16384}, wide.size() - offset);
            // 分片边界不拆开 UTF-16 代理对。
            if (offset + chunk < wide.size() && wide[offset + chunk - 1] >= 0xd800 && wide[offset + chunk - 1] <= 0xdbff) --chunk;
            DWORD written = 0;
            if (!WriteConsoleW(output, wide.data() + offset, static_cast<DWORD>(chunk), &written, nullptr) || written == 0)
                throw std::runtime_error("cannot write console output");
            offset += written;
        }
        return;
    }
#endif
    std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!std::cout) throw std::runtime_error("cannot write standard output");
}
}
