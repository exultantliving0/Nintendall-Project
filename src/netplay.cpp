#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#undef interface
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "netplay.hpp"

#include <SDL.h>
#include <zlib.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <random>

#include "detect.hpp"
#include "zip.hpp"

namespace {

constexpr uint8_t kMagic[4] = {'N', 'D', 'N', 'P'};
constexpr uint8_t kProtocol = 2;
enum Msg : uint8_t {
    MsgHello = 1,  // joining player -> host: version, name
    MsgWelcome,    // host -> player: player number, game, options, players
    MsgReject,     // host -> player: why not
    MsgPlayers,    // host -> players: names (also the lobby's keep-alive)
    MsgChunk,      // host -> player: a piece of the save state
    MsgChunkAck,   // player -> host: pieces received so far
    MsgReady,      // player -> host: save state loaded
    MsgGo,         // host -> player: frame 0 starts now
    MsgInput,      // presses (players -> host -> everyone else)
    MsgCheck,      // player -> host: a checksum of the game's state at a frame
    MsgDesync,     // host -> players: someone went out of sync
    MsgBye,        // leaving
    MsgPing,       // player -> host: keep-alive (in the lobby; link-cable games: always)
    MsgLink,       // link-cable games: a packet from an emulator (players -> host -> players)
    MsgLinkAck,    // link-cable games: packets received so far
};
constexpr size_t kChunkBytes = 1024;
constexpr uint32_t kChunkWindow = 64;  // pieces in flight per player
constexpr int kRedundancy = 16;        // each input packet repeats the last 16 frames
constexpr Uint32 kTimeoutMs = 10000;
constexpr int kCheckEvery = 180;       // frames between sync checks (3 seconds)
constexpr int kNever = INT_MAX;

#ifdef _WIN32
using Sock = SOCKET;
const Sock kNoSock = INVALID_SOCKET;
bool netInit() {
    static const bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
void closeSock(Sock s) { closesocket(s); }
#else
using Sock = int;
const Sock kNoSock = -1;
bool netInit() { return true; }
void closeSock(Sock s) { close(s); }
#endif

struct Addr {
    sockaddr_in sa{};
    bool operator==(const Addr& o) const {
        return sa.sin_addr.s_addr == o.sa.sin_addr.s_addr && sa.sin_port == o.sa.sin_port;
    }
};

struct Writer {
    std::vector<uint8_t> b;
    explicit Writer(Msg type, uint32_t session) {
        b.insert(b.end(), kMagic, kMagic + 4);
        b.push_back(kProtocol);
        b.push_back(type);
        u32(session);
    }
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) {
        b.push_back(uint8_t(v));
        b.push_back(uint8_t(v >> 8));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
    }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void str(const std::string& s) {
        const size_t n = std::min<size_t>(s.size(), 0xffff);
        u16(uint16_t(n));
        b.insert(b.end(), s.begin(), s.begin() + std::ptrdiff_t(n));
    }
    void bytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
    void input(const NetInput& in) {
        u16(in.buttons);
        for (int16_t a : in.axes) u16(uint16_t(a));
    }
};

struct Reader {
    const uint8_t* p;
    size_t n, i = 0;
    bool ok = true;
    Reader(const uint8_t* data, size_t size) : p(data), n(size) {}
    bool need(size_t k) {
        if (i + k > n) ok = false;
        return ok;
    }
    uint8_t u8() { return need(1) ? p[i++] : 0; }
    uint16_t u16() {
        if (!need(2)) return 0;
        const uint16_t v = uint16_t(p[i] | (p[i + 1] << 8));
        i += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v |= uint32_t(p[i + size_t(k)]) << (8 * k);
        i += 4;
        return v;
    }
    int32_t i32() { return int32_t(u32()); }
    std::string str() {
        const size_t len = u16();
        if (!need(len)) return "";
        std::string s(reinterpret_cast<const char*>(p + i), len);
        i += len;
        return s;
    }
    NetInput input() {
        NetInput in;
        in.buttons = u16();
        for (int16_t& a : in.axes) a = int16_t(u16());
        return in;
    }
};

// Link-cable packets to one computer: numbered, resent until it confirms them, handed on in order.
struct LinkChannel {
    uint32_t nextSeq = 0, acked = 0;
    std::deque<std::vector<uint8_t>> unacked;  // whole messages, numbers acked .. nextSeq-1
    uint32_t expect = 0;                        // the next number to hand on
    std::map<uint32_t, std::vector<uint8_t>> early;  // arrived while an earlier one is missing
    Uint32 lastResend = 0;
};
constexpr size_t kLinkResendCount = 64;
constexpr Uint32 kLinkResendMs = 120;

uint32_t crcOf(const std::vector<uint8_t>& d) {
    return uint32_t(crc32(crc32(0L, Z_NULL, 0), d.data(), uInt(d.size())));
}

}  // namespace

// ---------------------------------------------------------------------------

struct Netplay::Impl {
    Sock sock = kNoSock;
    uint32_t session = 0;

    // Joining
    Addr hostAddr;
    uint32_t nonce = 0;
    Uint32 started = 0, lastHello = 0, lastHeard = 0, lastPing = 0, lastReady = 0;
    std::vector<std::vector<uint8_t>> pieces;
    uint32_t piecesGot = 0, rawSize = 0;
    bool stateComplete = false, stateTaken = false, stateLoaded = false;
    std::vector<uint8_t> state;
    bool linkLoaded = false;
    LinkChannel hostLink;

    // Hosting
    struct Peer {
        Addr addr;
        uint32_t nonce = 0;
        int player = 0;
        Uint32 lastHeard = 0;
        uint32_t acked = 0, sent = 0;
        Uint32 lastProgress = 0;
        bool ready = false;
        bool playing = false;  // its presses have started coming in
        int frame = 0;
        Uint32 lastGo = 0;
        LinkChannel link;
    };
    std::vector<Peer> peers;
    std::vector<uint8_t> packed;  // the compressed save state
    uint32_t chunks = 0;
    Uint32 lastPlayers = 0;
    std::map<int, std::vector<std::pair<int, uint32_t>>> peerChecks;  // frame -> (player, crc)

    ~Impl() {
        if (sock != kNoSock) closeSock(sock);
    }

    bool open(uint16_t port) {
        if (!netInit()) return false;
        sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == kNoSock) return false;
        int big = 4 << 20;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof big);
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&big), sizeof big);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(port);
        if (::bind(sock, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            closeSock(sock);
            sock = kNoSock;
            return false;
        }
#ifdef _WIN32
        u_long on = 1;
        ioctlsocket(sock, FIONBIO, &on);
        // Don't report "port unreachable" from an earlier send as an error on the next receive.
        BOOL no = FALSE;
        DWORD ret = 0;
        WSAIoctl(sock, _WSAIOW(IOC_VENDOR, 12), &no, sizeof no, nullptr, 0, &ret, nullptr, nullptr);
#else
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
        return true;
    }

    void send(const Addr& to, const Writer& w) {
        if (sock == kNoSock) return;
        ::sendto(sock, reinterpret_cast<const char*>(w.b.data()), int(w.b.size()), 0,
                 reinterpret_cast<const sockaddr*>(&to.sa), sizeof to.sa);
    }

    // One received packet (false when there are no more).
    bool receive(std::vector<uint8_t>& buf, Addr& from) {
        if (sock == kNoSock) return false;
        buf.resize(65536);
        socklen_t len = sizeof from.sa;
        const auto n = ::recvfrom(sock, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                                  reinterpret_cast<sockaddr*>(&from.sa), &len);
        if (n < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAECONNRESET) {  // an earlier send was refused: keep going
                buf.clear();
                return true;
            }
#endif
            return false;
        }
        buf.resize(size_t(n));
        return true;
    }

    void linkSend(const Addr& to, LinkChannel& ch, int from, int dest, const std::vector<uint8_t>& data) {
        Writer w(MsgLink, session);
        w.u32(ch.nextSeq++);
        w.u8(uint8_t(from));
        w.u16(uint16_t(dest));
        w.bytes(data.data(), data.size());
        ch.unacked.push_back(w.b);
        send(to, w);
    }
    void linkResend(const Addr& to, LinkChannel& ch, Uint32 now) {
        if (ch.unacked.empty() || now - ch.lastResend < kLinkResendMs) return;
        ch.lastResend = now;
        for (size_t i = 0; i < ch.unacked.size() && i < kLinkResendCount; ++i)
            ::sendto(sock, reinterpret_cast<const char*>(ch.unacked[i].data()), int(ch.unacked[i].size()), 0,
                     reinterpret_cast<const sockaddr*>(&to.sa), sizeof to.sa);
    }

    Peer* peerAt(const Addr& a) {
        for (Peer& p : peers)
            if (p.addr == a) return &p;
        return nullptr;
    }
};

Netplay::Netplay() : net_(std::make_unique<Impl>()) { lastKnown_.fill(-1); }

Netplay::~Netplay() { leave(); }

std::vector<std::string> Netplay::takeMessages() {
    std::vector<std::string> m;
    m.swap(messages_);
    return m;
}

int Netplay::playerCount() const {
    return int(std::count_if(names_.begin(), names_.end(), [](const std::string& n) { return !n.empty(); }));
}

void Netplay::fail(const std::string& message) {
    if (phase_ == Phase::Error || phase_ == Phase::Ended) return;
    phase_ = Phase::Error;
    error_ = message;
}

void Netplay::end(const std::string& message) {
    if (phase_ == Phase::Error || phase_ == Phase::Ended) return;
    phase_ = Phase::Ended;
    error_ = message;
}

bool Netplay::host(const std::string& name, const GameInfo& game, std::string& error) {
    host_ = true;
    name_ = name;
    game_ = game;
    if (!net_->open(uint16_t(kNetplayPort))) {
        error = "Couldn't use port " + std::to_string(kNetplayPort) +
                ". Is another LAN game already running on this computer?";
        return false;
    }
    std::random_device rd;
    net_->session = rd() | 1u;
    names_.assign(kNetplayMaxPlayers, "");
    names_[0] = name;
    local_ = 0;
    phase_ = Phase::Lobby;
    return true;
}

bool Netplay::join(const std::string& target, const std::string& name, std::string& error) {
    host_ = false;
    name_ = name;
    std::string address = target;
    uint16_t port = uint16_t(kNetplayPort);
    const size_t colon = address.rfind(':');
    if (colon != std::string::npos && address.find(':') == colon) {
        const int p = std::atoi(address.c_str() + colon + 1);
        if (p > 0 && p < 65536) port = uint16_t(p);
        address = address.substr(0, colon);
    }
    if (!net_->open(0)) {
        error = "Couldn't open a network connection.";
        return false;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(address.c_str(), nullptr, &hints, &res) != 0 || !res) {
        error = "Couldn't find \"" + target + "\". Check the host's address.";
        return false;
    }
    std::memcpy(&net_->hostAddr.sa, res->ai_addr, sizeof net_->hostAddr.sa);
    freeaddrinfo(res);
    net_->hostAddr.sa.sin_port = htons(port);
    std::random_device rd;
    net_->nonce = rd();
    net_->started = SDL_GetTicks();
    phase_ = Phase::Connecting;
    return true;
}

void Netplay::leave() {
    if (!net_ || net_->sock == kNoSock) return;
    Writer w(MsgBye, net_->session);
    if (host_) {
        for (int i = 0; i < 3; ++i)
            for (auto& p : net_->peers) net_->send(p.addr, w);
    } else if (phase_ != Phase::Connecting) {
        for (int i = 0; i < 3; ++i) net_->send(net_->hostAddr, w);
    }
    closeSock(net_->sock);
    net_->sock = kNoSock;
}

// ---------------------------------------------------------------------------
// Messages

void Netplay::handle(const uint8_t* data, size_t size, const void* fromPtr) {
    const Addr& from = *static_cast<const Addr*>(fromPtr);
    if (size < 10 || std::memcmp(data, kMagic, 4) != 0 || data[4] != kProtocol) return;
    const Msg type = Msg(data[5]);
    Reader r(data + 6, size - 6);
    const uint32_t session = r.u32();
    const Uint32 now = SDL_GetTicks();

    if (host_) {
        if (type == MsgHello) {
            const std::string version = r.str(), name = r.str();
            const uint32_t nonce = r.u32();
            if (!r.ok) return;
            auto reject = [&](const std::string& why) {
                Writer w(MsgReject, 0);
                w.str(why);
                net_->send(from, w);
            };
            Impl::Peer* peer = net_->peerAt(from);
            if (!peer || peer->nonce != nonce) {
                if (version != NINTENDALL_VERSION)
                    return reject("The host has Nintendall " + std::string(NINTENDALL_VERSION) + " and you have " +
                                  version + ". Use the same version.");
                if (phase_ != Phase::Lobby && !(game_.link && phase_ == Phase::Playing))
                    return reject("The host has already started the game.");
                if (playerCount() >= game_.maxPlayers)
                    return reject("The game is full (" + std::to_string(game_.maxPlayers) + " players).");
                if (peer) {  // the same computer joining again
                    names_[size_t(peer->player)].clear();
                    net_->peers.erase(net_->peers.begin() + (peer - net_->peers.data()));
                }
                int slot = -1;
                for (int p = 1; p < kNetplayMaxPlayers && slot < 0; ++p)
                    if (names_[size_t(p)].empty()) slot = p;
                if (slot < 0) return reject("The game is full (4 players).");
                Impl::Peer np;
                np.addr = from;
                np.nonce = nonce;
                np.player = slot;
                np.lastHeard = now;
                net_->peers.push_back(np);
                names_[size_t(slot)] = name.empty() ? "Player " + std::to_string(slot + 1) : name;
                messages_.push_back(names_[size_t(slot)] + " joined");
                peer = &net_->peers.back();
                net_->lastPlayers = 0;  // tell everyone now
            }
            peer->lastHeard = now;
            Writer w(MsgWelcome, net_->session);
            w.u8(uint8_t(peer->player));
            w.i32(game_.system);
            w.u32(game_.crc);
            w.str(game_.title);
            w.u8(uint8_t(game_.window));
            w.u8(uint8_t(game_.delay));
            w.u8(game_.link ? 1 : 0);
            w.u8(uint8_t(game_.maxPlayers));
            w.u16(uint16_t(game_.options.size()));
            for (const auto& [k, v] : game_.options) {
                w.str(k);
                w.str(v);
            }
            net_->send(from, w);
            return;
        }
        Impl::Peer* peer = net_->peerAt(from);
        if (!peer || session != net_->session) return;
        peer->lastHeard = now;
        switch (type) {
        case MsgPing:
            if (r.u8() && r.ok && game_.link && phase_ == Phase::Playing && !peer->playing) {
                peer->playing = true;
                linkJoined_.push_back(peer->player);
            }
            break;
        case MsgLink:
        case MsgLinkAck:
            if (game_.link && phase_ == Phase::Playing) {
                if (!peer->playing) {
                    peer->playing = true;
                    linkJoined_.push_back(peer->player);
                }
                linkReceive(int(peer - net_->peers.data()), data, size);
            }
            break;
        case MsgChunkAck: {
            const uint32_t got = r.u32();
            if (r.ok && got > peer->acked) {
                peer->acked = got;
                peer->lastProgress = now;
            }
            break;
        }
        case MsgReady: peer->ready = true; break;
        case MsgInput: {
            const int theirFrame = r.i32();
            const int blocks = r.u8();
            for (int b = 0; b < blocks && r.ok; ++b) {
                const int player = r.u8(), first = r.i32(), n = r.u8();
                std::vector<NetInput> list;
                for (int i = 0; i < n; ++i) list.push_back(r.input());
                if (r.ok && player == peer->player && phase_ == Phase::Playing) receiveInputs(player, first, list);
            }
            if (r.ok) {
                peer->playing = true;
                peer->frame = theirFrame;
            }
            break;
        }
        case MsgCheck: {
            const int f = r.i32();
            const uint32_t crc = r.u32();
            if (r.ok) net_->peerChecks[f].push_back({peer->player, crc});
            break;
        }
        case MsgBye: {
            const int player = peer->player;
            const std::string who = names_[size_t(peer->player)];
            names_[size_t(peer->player)].clear();
            net_->peers.erase(net_->peers.begin() + (peer - net_->peers.data()));
            net_->lastPlayers = 0;
            if (phase_ == Phase::Lobby || game_.link) {
                messages_.push_back(who + " left");
                if (phase_ == Phase::Playing && game_.link) linkLeft_.push_back(player);
            } else {
                Writer w(MsgBye, net_->session);
                for (auto& p : net_->peers) net_->send(p.addr, w);
                end(who + " left. The LAN game ended.");
            }
            break;
        }
        default: break;
        }
        return;
    }

    // Joining: only the host talks to us.
    if (!(from == net_->hostAddr)) return;
    net_->lastHeard = now;
    switch (type) {
    case MsgWelcome: {
        if (phase_ != Phase::Connecting) break;
        GameInfo g;
        const int player = r.u8();
        g.system = r.i32();
        g.crc = r.u32();
        g.title = r.str();
        g.window = r.u8();
        g.delay = r.u8();
        g.link = r.u8() != 0;
        g.maxPlayers = r.u8();
        const int n = r.u16();
        for (int i = 0; i < n && r.ok; ++i) {
            std::string k = r.str(), v = r.str();
            g.options.push_back({k, v});
        }
        if (!r.ok || player <= 0 || player >= kNetplayMaxPlayers) break;
        net_->session = session;
        game_ = g;
        local_ = player;
        if (names_.empty()) names_.assign(kNetplayMaxPlayers, "");
        names_[size_t(player)] = name_;
        phase_ = Phase::Lobby;
        break;
    }
    case MsgReject:
        if (phase_ == Phase::Connecting) fail(r.str());
        break;
    default:
        if (session != net_->session || phase_ == Phase::Connecting) break;
        switch (type) {
        case MsgPlayers: {
            std::vector<std::string> names(kNetplayMaxPlayers);
            for (auto& s : names) s = r.str();
            if (!r.ok || (phase_ == Phase::Playing && !game_.link)) break;
            for (int p = 0; p < kNetplayMaxPlayers; ++p) {
                const std::string& was = names_[size_t(p)];
                if (p != local_ && was != names[size_t(p)]) {
                    if (!names[size_t(p)].empty() && was.empty()) messages_.push_back(names[size_t(p)] + " joined");
                    if (names[size_t(p)].empty() && !was.empty()) messages_.push_back(was + " left");
                }
            }
            names_ = names;
            break;
        }
        case MsgChunk: {
            const uint32_t raw = r.u32(), total = r.u32(), index = r.u32();
            if (!r.ok || total == 0 || total > (1u << 20) || index >= total || net_->stateComplete) {
                if (net_->stateComplete) {  // the host missed our last ack
                    Writer w(MsgChunkAck, net_->session);
                    w.u32(uint32_t(net_->pieces.size()));
                    net_->send(net_->hostAddr, w);
                }
                break;
            }
            if (net_->pieces.size() != total) {
                net_->pieces.assign(total, {});
                net_->piecesGot = 0;
                net_->rawSize = raw;
                phase_ = Phase::Syncing;
            }
            auto& piece = net_->pieces[index];
            if (piece.empty()) {
                piece.assign(data + 6 + r.i, data + size);
                if (piece.empty()) piece.push_back(0);  // (pieces are never empty)
                ++net_->piecesGot;
            }
            uint32_t contiguous = 0;
            while (contiguous < total && !net_->pieces[contiguous].empty()) ++contiguous;
            Writer w(MsgChunkAck, net_->session);
            w.u32(contiguous);
            net_->send(net_->hostAddr, w);
            if (net_->piecesGot == total) {
                std::vector<uint8_t> packed;
                for (auto& p : net_->pieces) packed.insert(packed.end(), p.begin(), p.end());
                net_->state.assign(net_->rawSize, 0);
                uLongf len = uLongf(net_->rawSize);
                if (uncompress(net_->state.data(), &len, packed.data(), uLong(packed.size())) != Z_OK ||
                    len != net_->rawSize) {
                    fail("The game from the host arrived damaged. Try again.");
                    break;
                }
                net_->stateComplete = true;
            }
            break;
        }
        case MsgGo: {
            if (game_.link) {  // link-cable game: our own game starts
                if (phase_ != Phase::Lobby || !net_->linkLoaded) break;
                std::vector<std::string> names(kNetplayMaxPlayers);
                for (auto& s : names) s = r.str();
                if (r.ok) names_ = names;
                phase_ = Phase::Playing;
                net_->lastPing = 0;  // say so now
                break;
            }
            if (phase_ != Phase::Syncing || !net_->stateLoaded) break;
            std::vector<std::string> names(kNetplayMaxPlayers);
            for (auto& s : names) s = r.str();
            if (!r.ok) break;
            names_ = names;
            phase_ = Phase::Playing;
            frame_ = 0;
            for (int p = 0; p < kNetplayMaxPlayers; ++p)
                lastKnown_[size_t(p)] = names_[size_t(p)].empty() ? kNever / 2 : game_.delay - 1;
            for (int f = 0; f < game_.delay; ++f) known_[f] = NetInputs{};
            ring_.assign(size_t(game_.window + 2), {});
            ringFrame_.assign(size_t(game_.window + 2), -1);
            break;
        }
        case MsgInput: {
            if (phase_ != Phase::Playing) break;
            const int hostFrame = r.i32();
            const int blocks = r.u8();
            for (int b = 0; b < blocks && r.ok; ++b) {
                const int player = r.u8(), first = r.i32(), n = r.u8();
                std::vector<NetInput> list;
                for (int i = 0; i < n; ++i) list.push_back(r.input());
                if (r.ok && player != local_ && player < kNetplayMaxPlayers) receiveInputs(player, first, list);
            }
            if (r.ok) remoteFrame_ = std::max(remoteFrame_, hostFrame);
            break;
        }
        case MsgLink:
        case MsgLinkAck:
            if (game_.link && phase_ == Phase::Playing) linkReceive(-1, data, size);
            break;
        case MsgDesync:
            if (!desyncShown_) {
                desyncShown_ = true;
                messages_.push_back("The game went out of sync with " + r.str() +
                                    ". Close it and start the LAN game again.");
            }
            break;
        case MsgBye: end(phase_ == Phase::Lobby ? "The host closed the LAN game." : "The host left. The LAN game ended."); break;
        default: break;
        }
    }
}

void Netplay::poll() {
    if (!net_ || net_->sock == kNoSock) return;
    std::vector<uint8_t> buf;
    Addr from;
    while (net_->receive(buf, from))
        if (!buf.empty()) handle(buf.data(), buf.size(), &from);

    const Uint32 now = SDL_GetTicks();
    if (host_) {
        // Lobby / starting: the player list (also keeps the connection alive).
        if ((phase_ == Phase::Lobby || phase_ == Phase::Syncing || (phase_ == Phase::Playing && game_.link)) &&
            now - net_->lastPlayers >= 1000) {
            net_->lastPlayers = now;
            Writer w(MsgPlayers, net_->session);
            for (const auto& n : names_) w.str(n);
            for (auto& p : net_->peers) net_->send(p.addr, w);
        }
        // Players who went quiet left.
        for (size_t i = 0; i < net_->peers.size(); ++i) {
            if (now - net_->peers[i].lastHeard < kTimeoutMs) continue;
            const int player = net_->peers[i].player;
            const std::string who = names_[size_t(player)];
            names_[size_t(player)].clear();
            net_->peers.erase(net_->peers.begin() + std::ptrdiff_t(i));
            net_->lastPlayers = 0;
            if (phase_ == Phase::Lobby || game_.link) {
                messages_.push_back(who + (phase_ == Phase::Lobby ? " left" : " lost the connection"));
                if (phase_ == Phase::Playing && game_.link) linkLeft_.push_back(player);
            } else if (phase_ == Phase::Syncing || phase_ == Phase::Playing) {
                Writer w(MsgBye, net_->session);
                for (auto& p : net_->peers) net_->send(p.addr, w);
                end("Lost the connection to " + who + ". The LAN game ended.");
            }
            break;
        }
        // Starting: send the save state, then go when everyone has loaded it.
        if (phase_ == Phase::Syncing) {
            bool allReady = true;
            for (auto& p : net_->peers) {
                if (p.acked < net_->chunks) {
                    if (now - p.lastProgress > 300) p.sent = p.acked;  // resend what's missing
                    while (p.sent < net_->chunks && p.sent < p.acked + kChunkWindow) {
                        const uint32_t i = p.sent++;
                        Writer w(MsgChunk, net_->session);
                        w.u32(uint32_t(net_->rawSize));
                        w.u32(net_->chunks);
                        w.u32(i);
                        const size_t at = size_t(i) * kChunkBytes;
                        w.bytes(net_->packed.data() + at, std::min(kChunkBytes, net_->packed.size() - at));
                        net_->send(p.addr, w);
                    }
                }
                allReady = allReady && p.ready;
            }
            if (allReady) {
                phase_ = Phase::Playing;
                frame_ = 0;
                for (int p = 0; p < kNetplayMaxPlayers; ++p)
                    lastKnown_[size_t(p)] = names_[size_t(p)].empty() ? kNever / 2 : game_.delay - 1;
                for (int f = 0; f < game_.delay; ++f) known_[f] = NetInputs{};
                ring_.assign(size_t(game_.window + 2), {});
                ringFrame_.assign(size_t(game_.window + 2), -1);
            }
        }
        // Link-cable game: packets not confirmed yet go again.
        if (phase_ == Phase::Playing && game_.link)
            for (auto& p : net_->peers) net_->linkResend(p.addr, p.link, now);
        // Playing: "go" until each player's presses come in (link-cable game: until they say they started).
        if (phase_ == Phase::Playing) {
            for (auto& p : net_->peers) {
                if (p.playing || now - p.lastGo < 200) continue;
                p.lastGo = now;
                Writer w(MsgGo, net_->session);
                for (const auto& n : names_) w.str(n);
                net_->send(p.addr, w);
            }
        }
        return;
    }

    // Joining
    if (phase_ == Phase::Connecting) {
        if (now - net_->started > kTimeoutMs) {
            fail("Couldn't reach the host. Check the address, and that the host is waiting for players.");
            return;
        }
        if (now - net_->lastHello >= 500) {
            net_->lastHello = now;
            Writer w(MsgHello, 0);
            w.str(NINTENDALL_VERSION);
            w.str(name_);
            w.u32(net_->nonce);
            net_->send(net_->hostAddr, w);
        }
        return;
    }
    if (phase_ == Phase::Lobby || phase_ == Phase::Syncing || phase_ == Phase::Playing) {
        if (now - net_->lastHeard > kTimeoutMs) {
            end(phase_ == Phase::Lobby ? "Lost the connection to the host."
                                       : "Lost the connection to the host. The LAN game ended.");
            return;
        }
        if ((phase_ != Phase::Playing || game_.link) && now - net_->lastPing >= 1000) {
            net_->lastPing = now;
            Writer w(MsgPing, net_->session);
            w.u8(phase_ == Phase::Playing ? 1 : 0);
            net_->send(net_->hostAddr, w);
        }
        if (phase_ == Phase::Playing && game_.link) net_->linkResend(net_->hostAddr, net_->hostLink, now);
        if (phase_ == Phase::Syncing && net_->stateLoaded && now - net_->lastReady >= 300) {
            net_->lastReady = now;
            Writer w(MsgReady, net_->session);
            net_->send(net_->hostAddr, w);
        }
    }
}

void Netplay::start(const std::vector<uint8_t>& state) {
    if (!host_ || phase_ != Phase::Lobby) return;
    uLongf len = compressBound(uLong(state.size()));
    net_->packed.assign(len, 0);
    if (compress2(net_->packed.data(), &len, state.data(), uLong(state.size()), 6) != Z_OK) {
        fail("Couldn't start the game.");
        return;
    }
    net_->packed.resize(len);
    net_->rawSize = uint32_t(state.size());
    net_->chunks = uint32_t((len + kChunkBytes - 1) / kChunkBytes);
    for (auto& p : net_->peers) {
        p.acked = p.sent = 0;
        p.ready = false;
        p.lastProgress = SDL_GetTicks();
    }
    phase_ = Phase::Syncing;
}

// ---------------------------------------------------------------------------
// Link-cable games

void Netplay::startLink() {
    if (!host_ || phase_ != Phase::Lobby || !game_.link) return;
    phase_ = Phase::Playing;
    for (auto& p : net_->peers) p.lastGo = 0;
}

void Netplay::linkLoaded() { net_->linkLoaded = true; }

void Netplay::linkSend(int to, const void* data, size_t size) {
    if (!game_.link || phase_ != Phase::Playing) return;
    std::vector<uint8_t> d(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    if (host_) linkDeliver(0, to, std::move(d));
    else net_->linkSend(net_->hostAddr, net_->hostLink, local_, to, d);
}

void Netplay::linkDeliver(int from, int to, std::vector<uint8_t> data) {
    // Host: a packet from `from` (0 = this computer), for `to`.
    for (auto& p : net_->peers)
        if (p.playing && p.player != from && (to == kLinkEveryone || to == p.player))
            net_->linkSend(p.addr, p.link, from, to, data);
    if (from != 0 && (to == 0 || to == kLinkEveryone)) linkIn_.push_back({from, std::move(data)});
}

void Netplay::linkReceive(int peerIndex, const uint8_t* data, size_t size) {
    LinkChannel& ch = peerIndex < 0 ? net_->hostLink : net_->peers[size_t(peerIndex)].link;
    const Addr to = peerIndex < 0 ? net_->hostAddr : net_->peers[size_t(peerIndex)].addr;
    const int sender = peerIndex < 0 ? 0 : net_->peers[size_t(peerIndex)].player;
    Reader r(data + 10, size - 10);
    if (data[5] == MsgLinkAck) {
        const uint32_t next = r.u32();
        while (r.ok && ch.acked < next && !ch.unacked.empty()) {
            ch.unacked.pop_front();
            ++ch.acked;
        }
        return;
    }
    auto take = [&](const std::vector<uint8_t>& msg) {
        Reader m(msg.data() + 14, msg.size() - 14);
        const int from = m.u8(), dest = m.u16();
        if (!m.ok) return;
        std::vector<uint8_t> payload(msg.begin() + 17, msg.end());
        if (host_) linkDeliver(sender, dest, std::move(payload));  // (players can't speak for others)
        else linkIn_.push_back({from, std::move(payload)});
    };
    const uint32_t seq = r.u32();
    if (r.ok && size >= 17) {
        if (seq == ch.expect) {
            take(std::vector<uint8_t>(data, data + size));
            ++ch.expect;
            for (auto it = ch.early.find(ch.expect); it != ch.early.end(); it = ch.early.find(ch.expect)) {
                take(it->second);
                ch.early.erase(it);
                ++ch.expect;
            }
        } else if (seq > ch.expect && seq - ch.expect < 4096) {
            ch.early.emplace(seq, std::vector<uint8_t>(data, data + size));
        }
    }
    Writer w(MsgLinkAck, net_->session);
    w.u32(ch.expect);
    net_->send(to, w);
}

std::vector<Netplay::LinkPacket> Netplay::takeLinkPackets() {
    std::vector<LinkPacket> v;
    v.swap(linkIn_);
    return v;
}

std::vector<int> Netplay::takeLinkJoined() {
    std::vector<int> v;
    v.swap(linkJoined_);
    return v;
}

std::vector<int> Netplay::takeLinkLeft() {
    std::vector<int> v;
    v.swap(linkLeft_);
    return v;
}

std::string Netplay::hostIp() const {
    if (host_) return "";
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &net_->hostAddr.sa.sin_addr, buf, sizeof buf);
    return buf;
}

bool Netplay::takeState(std::vector<uint8_t>& state) {
    if (host_ || !net_->stateComplete || net_->stateTaken) return false;
    net_->stateTaken = true;
    state.swap(net_->state);
    return true;
}

void Netplay::stateLoaded() { net_->stateLoaded = true; }

// ---------------------------------------------------------------------------
// Frames

NetInputs Netplay::inputsFor(int f) const {
    NetInputs in{};
    for (int p = 0; p < kNetplayMaxPlayers; ++p) {
        if (names_.size() <= size_t(p) || names_[size_t(p)].empty()) continue;
        const int known = lastKnown_[size_t(p)];
        const int at = f <= known ? f : known;  // not known yet: guess the last press seen
        const auto it = known_.find(at);
        if (it != known_.end()) in[size_t(p)] = it->second[size_t(p)];
    }
    return in;
}

void Netplay::receiveInputs(int player, int first, const std::vector<NetInput>& list) {
    auto& last = lastKnown_[size_t(player)];
    for (size_t i = 0; i < list.size(); ++i) {
        const int f = first + int(i);
        if (f <= last) continue;
        if (f != last + 1) break;  // a gap: the next packet repeats these frames
        known_[f][size_t(player)] = list[i];
        last = f;
        if (f < frame_) {  // that frame ran with a guess: go back if it was wrong
            const auto u = used_.find(f);
            if (u != used_.end() && u->second[size_t(player)] != list[i]) rollbackFrom_ = std::min(rollbackFrom_, f);
        }
    }
}

void Netplay::sendInputs() {
    auto block = [&](Writer& w, int player) {
        const int last = lastKnown_[size_t(player)];
        const int first = std::max(0, last - kRedundancy + 1);
        w.u8(uint8_t(player));
        w.i32(first);
        w.u8(uint8_t(last - first + 1));
        for (int f = first; f <= last; ++f) {
            const auto it = known_.find(f);
            w.input(it != known_.end() ? it->second[size_t(player)] : NetInput{});
        }
    };
    if (host_) {
        for (auto& peer : net_->peers) {
            Writer w(MsgInput, net_->session);
            w.i32(frame_);
            std::vector<int> others;
            for (int p = 0; p < kNetplayMaxPlayers; ++p)
                if (p != peer.player && !names_[size_t(p)].empty() && lastKnown_[size_t(p)] >= 0) others.push_back(p);
            w.u8(uint8_t(others.size()));
            for (int p : others) block(w, p);
            net_->send(peer.addr, w);
        }
    } else {
        Writer w(MsgInput, net_->session);
        w.i32(frame_);
        w.u8(1);
        block(w, local_);
        net_->send(net_->hostAddr, w);
    }
}

void Netplay::recordCheck(int frame, uint32_t crc) {
    if (host_) {
        checks_[frame] = crc;
        return;
    }
    Writer w(MsgCheck, net_->session);
    w.i32(frame);
    w.u32(crc);
    net_->send(net_->hostAddr, w);
}

void Netplay::checkSync(const Callbacks& cb) {
    (void)cb;
    // Host: compare the players' checksums with its own.
    if (host_) {
        for (auto it = net_->peerChecks.begin(); it != net_->peerChecks.end();) {
            const auto own = checks_.find(it->first);
            if (own == checks_.end()) {
                ++it;
                continue;
            }
            for (const auto& [player, crc] : it->second) {
                std::fprintf(stderr, "LAN: frame %d %s %s (rollbacks so far: %d)\n", it->first,
                             crc == own->second ? "in sync with" : "OUT OF SYNC with", names_[size_t(player)].c_str(),
                             rollbacks_);
                if (crc == own->second || desyncShown_) continue;
                desyncShown_ = true;
                const std::string who = names_[size_t(player)];
                messages_.push_back("The game went out of sync with " + who +
                                    ". Close it and start the LAN game again.");
                Writer w(MsgDesync, net_->session);
                w.str(who);
                for (auto& p : net_->peers) net_->send(p.addr, w);
            }
            it = net_->peerChecks.erase(it);
        }
        while (checks_.size() > 64) checks_.erase(checks_.begin());
    }
}

int Netplay::advance(const Callbacks& cb) {
    poll();
    if (phase_ != Phase::Playing) return 0;

    auto confirmed = [&] { return *std::min_element(lastKnown_.begin(), lastKnown_.end()); };
    int remote = remoteFrame_;
    if (host_)
        for (const auto& p : net_->peers) remote = std::max(remote, p.frame);
    const int want = remote - frame_ > 2 ? 2 : 1;  // behind the others: catch up

    int ran = 0;
    for (int k = 0; k < want; ++k) {
        // This computer's presses, for `delay` frames from now.
        const int target = frame_ + game_.delay;
        auto& mine = lastKnown_[size_t(local_)];
        if (mine < target) {
            const NetInput in = cb.sampleLocal();
            for (int f = mine + 1; f <= target; ++f) known_[f][size_t(local_)] = in;
            mine = target;
        }
        if (frame_ > confirmed() + game_.window) {  // waiting for the others' presses
            if (waitFrame_ != frame_) {
                waitFrame_ = frame_;
                ++waits_;
            }
            break;
        }

        // A guess was wrong: go back and play those frames again.
        if (rollbackFrom_ < frame_) {
            const int from = rollbackFrom_;
            rollbackFrom_ = kNever;
            ++rollbacks_;
            const size_t slot = size_t(from) % ring_.size();
            if (ring_.empty() || ringFrame_[slot] != from || !cb.load(ring_[slot])) {
                end("The LAN game went out of step and had to stop.");
                return ran;
            }
            for (int f = from; f < frame_; ++f) {
                if (f > from) {
                    const size_t s = size_t(f) % ring_.size();
                    ring_[s] = cb.save();
                    ringFrame_[s] = f;
                }
                const NetInputs in = inputsFor(f);
                used_[f] = in;
                cb.run(in, true);
            }
        }

        // (Sync checks need rollbacks' save states; see below. Without a rollback window - N64 - there
        // are none: its save states also differ between computers whose games are in step.)
        if (game_.window > 0) {
            const size_t s = size_t(frame_) % ring_.size();
            ring_[s] = cb.save();
            ringFrame_[s] = frame_;
        }
        const NetInputs in = inputsFor(frame_);
        used_[frame_] = in;
        cb.run(in, false);
        ++frame_;
        ++ran;
        if (frame_ % 600 == 0)
            std::fprintf(stderr, "LAN: frame %d (%d frames waited for other players, rollbacks %d)\n", frame_, waits_,
                         rollbacks_);
    }

    // With rollbacks: check a frame once every press before it is known (and played right), from
    // the save state taken at its start - the same frames on every computer.
    while (game_.window > 0) {
        const int c = (lastCheck_ / kCheckEvery + 1) * kCheckEvery;
        if (c > confirmed() + 1 || c >= frame_ || rollbackFrom_ < c) break;
        lastCheck_ = c;
        const size_t slot = size_t(c) % ring_.size();
        if (ringFrame_[slot] == c) recordCheck(c, crcOf(ring_[slot]));
    }

    sendInputs();
    checkSync(cb);

    // Forget old frames: keep what can still be repeated to other players (the last 16 frames of
    // each), guessed from, or played again in a rollback.
    int keepFrom = frame_ - game_.window - 2;
    for (int p = 0; p < kNetplayMaxPlayers; ++p)
        if (lastKnown_[size_t(p)] < kNever / 2) keepFrom = std::min(keepFrom, lastKnown_[size_t(p)] - kRedundancy);
    while (!known_.empty() && known_.begin()->first < keepFrom - 8) known_.erase(known_.begin());
    while (!used_.empty() && used_.begin()->first < std::min(confirmed(), frame_) - 8) used_.erase(used_.begin());
    return ran;
}

// ---------------------------------------------------------------------------

uint32_t netplayGameCrc(const std::string& pathUtf8) {
    const std::filesystem::path path = std::filesystem::u8path(pathUtf8);
    uLong crc = crc32(0L, Z_NULL, 0);
    const Detection det = detectGame(path);
    if (!det.zipEntry.empty()) {
        ZipFile z;
        if (!z.open(path)) return 0;
        for (const auto& e : z.entries()) {
            if (e.name != det.zipEntry) continue;
            std::vector<uint8_t> data;
            if (!z.read(e, data)) return 0;
            return uint32_t(crc32(crc, data.data(), uInt(data.size())));
        }
        return 0;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        const auto n = f.gcount();
        if (n > 0) crc = crc32(crc, reinterpret_cast<const Bytef*>(buf.data()), uInt(n));
    }
    return uint32_t(crc);
}
