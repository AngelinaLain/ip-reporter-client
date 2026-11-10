#pragma once
#include <string_view>

namespace ip_reporter {
// 终端按原生 Unicode 输出；管道或文件始终保留无 BOM 的 UTF-8 JSON。
void print_stdout_utf8(std::string_view text);
}
