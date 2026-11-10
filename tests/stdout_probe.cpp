#include "ip_reporter/console.hpp"

#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr std::string_view sample = u8"接口：以太网 | IPv4 / IPv6";

#ifdef _WIN32
// 只操作 Python 为本测试新建的隐藏控制台，不接触用户当前的终端或网卡。
int verify_console(unsigned int code_page) {
    HANDLE output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (output == INVALID_HANDLE_VALUE) return 11;
    const auto previous_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    struct Restore {
        HANDLE output, previous_handle;
        ~Restore() { SetStdHandle(STD_OUTPUT_HANDLE, previous_handle); CloseHandle(output); }
    } restore{output, previous_handle};
    if (!SetStdHandle(STD_OUTPUT_HANDLE, output) || !SetConsoleOutputCP(code_page)) return 12;
    DWORD mode = 0;
    if (!GetConsoleMode(output, &mode)) return 13;
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(output, &info) || info.dwSize.X < 60) return 14;
    const COORD origin{0, 0};
    DWORD written = 0;
    if (!FillConsoleOutputCharacterW(output, L' ', info.dwSize.X, origin, &written) ||
        !FillConsoleOutputAttribute(output, info.wAttributes, info.dwSize.X, origin, &written) ||
        !SetConsoleCursorPosition(output, origin)) return 15;

    ip_reporter::print_stdout_utf8(sample);
    if (GetConsoleOutputCP() != code_page) return 16;

    std::vector<CHAR_INFO> cells(static_cast<std::size_t>(info.dwSize.X));
    const COORD size{info.dwSize.X, 1};
    SMALL_RECT area{0, 0, static_cast<SHORT>(info.dwSize.X - 1), 0};
    if (!ReadConsoleOutputW(output, cells.data(), size, origin, &area)) return 17;
    std::wstring actual;
    for (const auto& cell : cells) {
        // 中文全角字符占两个屏幕格，尾格不能当作第二个字符读取。
        if ((cell.Attributes & COMMON_LVB_TRAILING_BYTE) == 0) actual.push_back(cell.Char.UnicodeChar);
    }
    while (!actual.empty() && actual.back() == L' ') actual.pop_back();
    return actual == L"接口：以太网 | IPv4 / IPv6" ? 0 : 18;
}
#endif
}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    if (argc == 3) {
        const std::string mode = argv[1];
        const std::string page = argv[2];
        const unsigned int code_page = page == "936" ? 936u : page == "437" ? 437u : 0u;
        if (!code_page || !IsValidCodePage(code_page)) return 10;
        if (mode == "--verify-console") return verify_console(code_page);
        if (mode == "--pipe-codepage") {
            if (!SetConsoleOutputCP(code_page)) return 20;
            DWORD console_mode = 0;
            if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &console_mode)) return 21;
            ip_reporter::print_stdout_utf8(sample);
            return GetConsoleOutputCP() == code_page ? 0 : 22;
        }
        return 23;
    }
#endif
    if (argc != 1) return 2;
    ip_reporter::print_stdout_utf8(sample);
    return 0;
}
