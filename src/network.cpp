#include "ip_reporter/network.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__)
#include <netpacket/packet.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <net/if_dl.h>
#endif
#endif

namespace ip_reporter {
namespace {
// 网卡物理地址按小写十六进制输出；隧道等没有有效 MAC 的接口使用空字符串。
std::string format_mac(const unsigned char* bytes, std::size_t length, std::size_t available) {
    if (!bytes || length < 6 || length > 20 || length > available ||
        std::all_of(bytes, bytes + length, [](unsigned char value) { return value == 0; })) return {};
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < length; ++index) {
        if (index) result << ':';
        result << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return result.str();
}
#ifdef _WIN32
struct Winsock {
    Winsock() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed"); }
    ~Winsock() { WSACleanup(); }
};
std::string utf8(const wchar_t* input) {
    if (!input || !*input) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, input, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw std::runtime_error("cannot convert interface name");
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input, -1, result.data(), length, nullptr, nullptr);
    result.pop_back();
    return result;
}
#endif
void canonicalize(Snapshot& s) {
    for (auto& iface : s.interfaces) {
        for (auto* list : {&iface.ipv4, &iface.ipv6}) {
            std::sort(list->begin(), list->end());
            list->erase(std::unique(list->begin(), list->end()), list->end());
        }
    }
    std::sort(s.interfaces.begin(), s.interfaces.end(), [](const auto& a, const auto& b) {
        return std::tie(a.name, a.id, a.mac, a.ipv4, a.ipv6) < std::tie(b.name, b.id, b.mac, b.ipv4, b.ipv6);
    });
}
void append_address(InterfaceAddresses& iface, const sockaddr* address) {
    std::array<char, INET6_ADDRSTRLEN> buffer{};
    const int family = address->sa_family;
    const void* bytes = nullptr;
    if (family == AF_INET) bytes = &reinterpret_cast<const sockaddr_in*>(address)->sin_addr;
    else if (family == AF_INET6) bytes = &reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr;
    else return;
    if (!inet_ntop(family, bytes, buffer.data(), static_cast<unsigned int>(buffer.size()))) return;
    const auto version = family == AF_INET ? 4 : 6;
    if (is_reportable_address(buffer.data(), version)) (version == 4 ? iface.ipv4 : iface.ipv6).emplace_back(buffer.data());
}
}

// 排除回环、未指定、链路本地和组播地址；保留 RFC1918 私网、CGNAT 和 IPv6 ULA。
bool is_reportable_address(const std::string& value, int family) {
    std::array<unsigned char, 16> bytes{};
    if (family != 4 && family != 6) return false;
    if (inet_pton(family == 4 ? AF_INET : AF_INET6, value.c_str(), bytes.data()) != 1) return false;
    if (family == 4) {
        return bytes[0] != 0 && bytes[0] != 127 && bytes[0] < 224 &&
            !(bytes[0] == 169 && bytes[1] == 254);
    }
    if (bytes[0] == 0xff || (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80)) return false;
    const bool zero_prefix = std::all_of(bytes.begin(), bytes.begin() + 15, [](auto x) { return x == 0; });
    if (zero_prefix && (bytes[15] == 0 || bytes[15] == 1)) return false;
    // IPv4 映射地址不应在 IPv6 列表中重复出现。
    if (std::all_of(bytes.begin(), bytes.begin() + 10, [](auto x) { return x == 0; }) && bytes[10] == 0xff && bytes[11] == 0xff) return false;
    return true;
}

Snapshot collect_snapshot() {
    Snapshot result;
    std::map<std::string, InterfaceAddresses> interfaces;
#ifdef _WIN32
    static Winsock winsock;
    result.os = "windows";
    wchar_t hostname[256]{};
    DWORD hostname_size = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, hostname, &hostname_size)) throw std::runtime_error("cannot read hostname");
    result.hostname = utf8(hostname);
    ULONG size = 15000;
    std::vector<unsigned char> storage(size);
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3; ++attempt) {
        status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()), &size);
        if (status != ERROR_BUFFER_OVERFLOW) break;
        if (size > 16 * 1024 * 1024) throw std::runtime_error("adapter enumeration exceeds limit");
        storage.resize(size);
    }
    if (status != NO_ERROR && status != ERROR_NO_DATA) throw std::runtime_error("cannot enumerate network adapters");
    if (status == NO_ERROR) {
        for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()); adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            const std::string key = adapter->AdapterName;
            auto& iface = interfaces[key];
            iface.id = key;
            iface.name = utf8(adapter->FriendlyName);
            if (iface.name.empty()) iface.name = key;
            iface.mac = format_mac(adapter->PhysicalAddress, adapter->PhysicalAddressLength, sizeof(adapter->PhysicalAddress));
            for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
                if (address->DadState != IpDadStatePreferred && address->DadState != IpDadStateDeprecated) continue;
                if (address->Address.lpSockaddr) append_address(iface, address->Address.lpSockaddr);
            }
        }
    }
#else
#if defined(__APPLE__)
    result.os = "macos";
#elif defined(__FreeBSD__)
    result.os = "freebsd";
#elif defined(__linux__)
    result.os = "linux";
#else
    result.os = "posix";
#endif
    std::array<char, 256> hostname{};
    if (gethostname(hostname.data(), hostname.size() - 1) != 0) throw std::runtime_error("cannot read hostname");
    result.hostname = hostname.data();
    ifaddrs* raw = nullptr;
    if (getifaddrs(&raw) != 0) throw std::runtime_error("cannot enumerate network adapters");
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> owner(raw, freeifaddrs);
    for (auto* item = raw; item; item = item->ifa_next) {
        if (!item->ifa_addr || !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK)) continue;
        auto& iface = interfaces[item->ifa_name];
        iface.name = item->ifa_name;
        // POSIX 以系统接口名作为稳定选择键；重命名接口后需重新选择 DDNS 绑定。
        iface.id = item->ifa_name;
        const auto family = item->ifa_addr->sa_family;
        if (family == AF_INET || family == AF_INET6) append_address(iface, item->ifa_addr);
#if defined(__linux__)
        else if (family == AF_PACKET) {
            const auto* link = reinterpret_cast<const sockaddr_ll*>(item->ifa_addr);
            iface.mac = format_mac(link->sll_addr, link->sll_halen, sizeof(link->sll_addr));
        }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        else if (family == AF_LINK) {
            const auto* link = reinterpret_cast<const sockaddr_dl*>(item->ifa_addr);
            const auto offset = offsetof(sockaddr_dl, sdl_data) + link->sdl_nlen;
            if (link->sdl_len >= offset) {
                iface.mac = format_mac(reinterpret_cast<const unsigned char*>(LLADDR(link)), link->sdl_alen,
                    static_cast<std::size_t>(link->sdl_len) - offset);
            }
        }
#endif
    }
#endif
    for (auto& pair : interfaces) {
        if (!pair.second.ipv4.empty() || !pair.second.ipv6.empty()) result.interfaces.push_back(std::move(pair.second));
    }
    canonicalize(result);
    return result;
}

std::string address_fingerprint(const Snapshot& snapshot) {
    auto copy = snapshot;
    canonicalize(copy);
    auto data = nlohmann::json::array();
    for (const auto& iface : copy.interfaces) data.push_back({{"id", iface.id}, {"name", iface.name}, {"mac", iface.mac}, {"ipv4", iface.ipv4}, {"ipv6", iface.ipv6}});
    return data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

nlohmann::json make_payload(const Config& config, const Snapshot& snapshot, const std::string& event,
                           std::uint64_t sequence, std::uint64_t uptime_seconds) {
    auto copy = snapshot;
    canonicalize(copy);
    std::set<std::string> ipv4, ipv6;
    auto interfaces = nlohmann::json::array();
    for (const auto& iface : copy.interfaces) {
        ipv4.insert(iface.ipv4.begin(), iface.ipv4.end());
        ipv6.insert(iface.ipv6.begin(), iface.ipv6.end());
        interfaces.push_back({{"id", iface.id}, {"name", iface.name}, {"mac", iface.mac}, {"ipv4", iface.ipv4}, {"ipv6", iface.ipv6}});
    }
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream timestamp;
    timestamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return {{"schema_version", 1}, {"agent_version", IP_REPORTER_VERSION}, {"device_id", config.device_id},
        {"timestamp", timestamp.str()}, {"sequence", sequence}, {"event", event}, {"hostname", snapshot.hostname},
        {"os", snapshot.os}, {"ipv4", ipv4}, {"ipv6", ipv6}, {"interfaces", interfaces},
        {"uptime_seconds", uptime_seconds}, {"status", "alive"}};
}
}
