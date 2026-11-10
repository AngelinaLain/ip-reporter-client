#pragma once
#include "config.hpp"
#include <cstdint>
#include <vector>

namespace ip_reporter {
struct InterfaceAddresses {
    std::string name;
    std::vector<std::string> ipv4, ipv6;
    std::string id;
    std::string mac;
};
struct Snapshot {
    std::string hostname, os;
    std::vector<InterfaceAddresses> interfaces;
};
Snapshot collect_snapshot();
bool is_reportable_address(const std::string& address, int family);
std::string address_fingerprint(const Snapshot& snapshot);
nlohmann::json make_payload(const Config&, const Snapshot&, const std::string& event,
                            std::uint64_t sequence, std::uint64_t uptime_seconds);
}
