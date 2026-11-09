#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#undef interface  // a Windows macro; VkRender has a function with that name
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

#include "lan.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include "core.hpp"

LanApi lanApi(const LibretroCore& core) {
    LanApi api;
    api.setup = reinterpret_cast<decltype(api.setup)>(core.symbol("nintendall_lan_setup"));
    api.start = reinterpret_cast<decltype(api.start)>(core.symbol("nintendall_lan_start"));
    api.setDelay = reinterpret_cast<decltype(api.setDelay)>(core.symbol("nintendall_lan_set_delay"));
    api.status = reinterpret_cast<decltype(api.status)>(core.symbol("nintendall_lan_status"));
    if (!api.ok()) api = LanApi{};
    return api;
}

LanStatus readLanStatus(const LanApi& api) {
    LanStatus st;
    if (!api.ok()) return st;
    std::string buf(16384, '\0');
    st.state = LanState(std::clamp(api.status(buf.data(), unsigned(buf.size())), 0, 6));
    buf.resize(std::char_traits<char>::length(buf.c_str()));

    std::istringstream in(buf);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "delay") st.delay = unsigned(std::max(1, std::atoi(value.c_str())));
        else if (key == "code") st.code = value;
        else if (key == "codestate") st.codeState = value;
        else if (key == "host") st.hostName = value;
        else if (key == "player") st.players.push_back(value);
        else if (key == "error") st.error = value;
        else if (key == "errorkind") st.errorKind = value;
        else if (key == "hostgameid") st.hostGameId = value;
        else if (key == "hostgame") st.hostGame = value;
        else if (key == "progress") st.progress = value;
        else if (key == "message") st.messages.push_back(value);
    }
    return st;
}

namespace {

bool usable(uint32_t hostOrderIp) {
    const uint32_t a = hostOrderIp >> 24, b = (hostOrderIp >> 16) & 0xFF;
    if (a == 127 || a == 0) return false;      // loopback / none
    if (a == 169 && b == 254) return false;    // no DHCP answer
    return true;
}

std::string ipText(uint32_t hostOrderIp) {
    return std::to_string(hostOrderIp >> 24) + "." + std::to_string((hostOrderIp >> 16) & 0xFF) + "." +
           std::to_string((hostOrderIp >> 8) & 0xFF) + "." + std::to_string(hostOrderIp & 0xFF);
}

// Home / office networks first (192.168.x, 10.x, 172.16-31.x), then others (e.g. a VPN's).
int rank(uint32_t ip) {
    const uint32_t a = ip >> 24, b = (ip >> 16) & 0xFF;
    if (a == 192 && b == 168) return 0;
    if (a == 10) return 1;
    if (a == 172 && b >= 16 && b < 32) return 1;
    return 2;
}

}  // namespace

std::vector<std::string> localAddresses() {
    std::vector<std::pair<int, uint32_t>> found;  // (rank, address)
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                                                GAA_FLAG_INCLUDE_GATEWAYS,
                                   nullptr, list, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                                              GAA_FLAG_INCLUDE_GATEWAYS,
                                 nullptr, list, &size);
    }
    if (r == NO_ERROR) {
        for (auto* a = list; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
                // The adapter with a gateway is the one that goes to the router: list it first.
                if (usable(ip)) found.push_back({rank(ip) - (a->FirstGatewayAddress ? 10 : 0), ip});
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* a = list; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
            if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
            const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr);
            if (usable(ip)) found.push_back({rank(ip), ip});
        }
        freeifaddrs(list);
    }
#endif
    std::stable_sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    std::vector<std::string> out;
    for (const auto& f : found) {
        const std::string s = ipText(f.second);
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
    return out;
}

std::string defaultPlayerName() {
#ifdef _WIN32
    const char* n = std::getenv("USERNAME");
#else
    const char* n = std::getenv("USER");
#endif
    std::string name = n ? n : "";
    if (name.size() > 20) name.resize(20);
    return name.empty() ? "Player" : name;
}
