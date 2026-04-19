/*
 * ONQL C++ Driver - Implementation
 */

#include "onql/client.hpp"

#include <cstring>
#include <random>
#include <algorithm>

/* ------------------------------------------------------------------ */
/* Platform socket abstraction                                         */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")

  static struct WsaInit {
      WsaInit()  { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); }
      ~WsaInit() { WSACleanup(); }
  } wsaInit_;

  #define SOCK_CLOSE(s) closesocket(s)
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <errno.h>
  #define SOCK_CLOSE(s) ::close(s)
#endif

static constexpr char EOM   = '\x04';
static constexpr char DELIM = '\x1E';
static constexpr int  RID_LEN = 8;

namespace onql {

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static std::string simple_json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

struct PathParts {
    std::string db;
    std::string table;
    std::string id;
};

static PathParts parsePath(const std::string& path, bool requireId) {
    if (path.empty()) {
        throw std::invalid_argument(
            "Path must be a non-empty string like \"db.table\" or \"db.table.id\"");
    }
    auto dot1 = path.find('.');
    if (dot1 == std::string::npos || dot1 == 0 || dot1 == path.size() - 1) {
        throw std::invalid_argument("Path \"" + path + "\" must contain at least \"db.table\"");
    }
    auto dot2 = path.find('.', dot1 + 1);
    PathParts p;
    p.db = path.substr(0, dot1);
    if (dot2 == std::string::npos) {
        p.table = path.substr(dot1 + 1);
    } else {
        p.table = path.substr(dot1 + 1, dot2 - dot1 - 1);
        p.id    = path.substr(dot2 + 1);
    }
    if (p.table.empty()) {
        throw std::invalid_argument("Path \"" + path + "\" must contain at least \"db.table\"");
    }
    if (requireId && p.id.empty()) {
        throw std::invalid_argument(
            "Path \"" + path + "\" must include a record id: \"db.table.id\"");
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* Client – construction / destruction / move                          */
/* ------------------------------------------------------------------ */

Client::Client()
    : sock_(SOCK_INVALID)
    , defaultTimeout_(std::chrono::seconds(10))
    , running_(false)
{}

Client::~Client() { close(); }

Client::Client(Client&& o) noexcept
    : sock_(o.sock_)
    , defaultTimeout_(o.defaultTimeout_)
    , running_(o.running_.load())
    , recvBuf_(std::move(o.recvBuf_))
    , pending_(std::move(o.pending_))
{
    o.sock_ = SOCK_INVALID;
    o.running_ = false;
    if (o.readerThread_.joinable()) {
        readerThread_ = std::move(o.readerThread_);
    }
}

Client& Client::operator=(Client&& o) noexcept {
    if (this != &o) {
        close();
        sock_           = o.sock_;
        defaultTimeout_ = o.defaultTimeout_;
        running_        = o.running_.load();
        recvBuf_        = std::move(o.recvBuf_);
        pending_        = std::move(o.pending_);
        o.sock_    = SOCK_INVALID;
        o.running_ = false;
        if (o.readerThread_.joinable())
            readerThread_ = std::move(o.readerThread_);
    }
    return *this;
}

/* ------------------------------------------------------------------ */
/* connect()                                                           */
/* ------------------------------------------------------------------ */

Client Client::connect(const std::string& host, int port,
                        std::chrono::seconds timeout)
{
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res)
        throw ConnectionError("Failed to resolve host: " + host);

    sock_t sock = SOCK_INVALID;
    for (auto rp = res; rp; rp = rp->ai_next) {
        sock = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == SOCK_INVALID) continue;
        if (::connect(sock, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0)
            break;
        SOCK_CLOSE(sock);
        sock = SOCK_INVALID;
    }
    freeaddrinfo(res);

    if (sock == SOCK_INVALID)
        throw ConnectionError("Could not connect to " + host + ":" + port_str);

    Client c;
    c.sock_           = sock;
    c.defaultTimeout_ = timeout;
    c.running_        = true;
    c.readerThread_   = std::thread(&Client::readerLoop, &c);

    return c;
}

/* ------------------------------------------------------------------ */
/* generateRequestId()                                                 */
/* ------------------------------------------------------------------ */

std::string Client::generateRequestId() {
    static thread_local std::mt19937 rng(
        static_cast<unsigned>(std::random_device{}() ^
                              std::hash<std::thread::id>{}(std::this_thread::get_id())));
    static constexpr char hex[] = "0123456789abcdef";
    std::string id(RID_LEN, '\0');
    for (int i = 0; i < RID_LEN; ++i)
        id[i] = hex[rng() % 16];
    return id;
}

/* ------------------------------------------------------------------ */
/* sendRaw()                                                           */
/* ------------------------------------------------------------------ */

void Client::sendRaw(const std::string& data) {
    const char* ptr = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
        int n = ::send(sock_, ptr, static_cast<int>(remaining), 0);
        if (n <= 0)
            throw ConnectionError("Send failed");
        ptr       += n;
        remaining -= static_cast<size_t>(n);
    }
}

/* ------------------------------------------------------------------ */
/* readerLoop()                                                        */
/* ------------------------------------------------------------------ */

void Client::readerLoop() {
    char buf[4096];

    while (running_) {
        int n = ::recv(sock_, buf, sizeof(buf), 0);
        if (n <= 0) {
            running_ = false;
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [rid, pr] : pending_) {
                pr->error = true;
                pr->ready = true;
                pr->cv.notify_all();
            }
            return;
        }

        std::lock_guard<std::mutex> lk(mu_);
        recvBuf_.append(buf, static_cast<size_t>(n));

        for (;;) {
            auto eom_pos = recvBuf_.find(EOM);
            if (eom_pos == std::string::npos)
                break;

            std::string frame = recvBuf_.substr(0, eom_pos);
            recvBuf_.erase(0, eom_pos + 1);

            auto d1 = frame.find(DELIM);
            if (d1 == std::string::npos) continue;
            auto d2 = frame.find(DELIM, d1 + 1);
            if (d2 == std::string::npos) continue;

            std::string rid     = frame.substr(0, d1);
            std::string source  = frame.substr(d1 + 1, d2 - d1 - 1);
            std::string payload = frame.substr(d2 + 1);

            auto pr_it = pending_.find(rid);
            if (pr_it != pending_.end()) {
                auto pr = pr_it->second;
                pr->response.requestId = std::move(rid);
                pr->response.source    = std::move(source);
                pr->response.payload   = std::move(payload);
                pr->ready = true;
                pr->cv.notify_all();
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* sendRequest()                                                       */
/* ------------------------------------------------------------------ */

Response Client::sendRequest(const std::string& keyword,
                              const std::string& payload,
                              std::chrono::seconds timeout)
{
    if (timeout.count() == 0)
        timeout = defaultTimeout_;

    if (!running_)
        throw ConnectionError("Client is not connected.");

    std::string rid = generateRequestId();

    auto pr = std::make_shared<PendingRequest>();

    std::string frame;
    frame.reserve(rid.size() + 1 + keyword.size() + 1 + payload.size() + 1);
    frame += rid;
    frame += DELIM;
    frame += keyword;
    frame += DELIM;
    frame += payload;
    frame += EOM;

    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_[rid] = pr;
    }

    try {
        sendRaw(frame);
    } catch (...) {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.erase(rid);
        throw;
    }

    {
        std::unique_lock<std::mutex> lk(mu_);
        bool ok = pr->cv.wait_for(lk, timeout, [&] { return pr->ready; });

        pending_.erase(rid);

        if (!ok)
            throw TimeoutError("Request " + rid + " timed out.");
        if (pr->error)
            throw ConnectionError("Connection lost.");
    }

    return std::move(pr->response);
}

/* ------------------------------------------------------------------ */
/* close()                                                             */
/* ------------------------------------------------------------------ */

void Client::close() {
    bool was_running = running_.exchange(false);

    if (sock_ != SOCK_INVALID) {
        SOCK_CLOSE(sock_);
        sock_ = SOCK_INVALID;
    }

    if (was_running && readerThread_.joinable()) {
        readerThread_.join();
    }

    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [rid, pr] : pending_) {
        pr->error = true;
        pr->ready = true;
        pr->cv.notify_all();
    }
    pending_.clear();
}

/* ------------------------------------------------------------------ */
/* ORM-style API                                                       */
/* ------------------------------------------------------------------ */

/* Skip whitespace at position p; returns new position. */
static size_t skip_ws(const std::string& s, size_t p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' ||
                             s[p] == '\n' || s[p] == '\r')) ++p;
    return p;
}

/* Extract a JSON value starting at position p in `s`. */
static bool extract_json_value(const std::string& s, size_t p,
                                size_t& out_start, size_t& out_end) {
    p = skip_ws(s, p);
    if (p >= s.size()) return false;
    out_start = p;
    char c = s[p];
    if (c == '"') {
        ++p;
        while (p < s.size()) {
            if (s[p] == '\\' && p + 1 < s.size()) p += 2;
            else if (s[p] == '"') { ++p; break; }
            else ++p;
        }
    } else if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 1;
        ++p;
        while (p < s.size() && depth > 0) {
            if (s[p] == '"') {
                ++p;
                while (p < s.size()) {
                    if (s[p] == '\\' && p + 1 < s.size()) p += 2;
                    else if (s[p] == '"') { ++p; break; }
                    else ++p;
                }
            } else {
                if (s[p] == open)  ++depth;
                if (s[p] == close) --depth;
                ++p;
            }
        }
    } else {
        while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']' &&
               s[p] != ' ' && s[p] != '\t' && s[p] != '\n' && s[p] != '\r')
            ++p;
    }
    out_end = p;
    return out_end > out_start;
}

static std::string find_value(const std::string& raw, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = raw.find(pat);
    while (p != std::string::npos) {
        size_t c = skip_ws(raw, p + pat.size());
        if (c < raw.size() && raw[c] == ':') {
            size_t vs, ve;
            if (extract_json_value(raw, c + 1, vs, ve))
                return raw.substr(vs, ve - vs);
            return {};
        }
        p = raw.find(pat, p + 1);
    }
    return {};
}

std::string Client::processResult(const std::string& raw) {
    std::string err = find_value(raw, "error");
    if (!err.empty() && err != "null" && err != "false" && err != "\"\"") {
        if (err.size() >= 2 && err.front() == '"' && err.back() == '"')
            err = err.substr(1, err.size() - 2);
        throw std::runtime_error(err);
    }
    return find_value(raw, "data");
}

std::string Client::insert(const std::string& path,
                           const std::string& recordJson) {
    PathParts p = parsePath(path, false);
    std::string payload;
    payload.reserve(64 + path.size() + recordJson.size());
    payload  = "{\"db\":\"";
    payload += simple_json_escape(p.db);
    payload += "\",\"table\":\"";
    payload += simple_json_escape(p.table);
    payload += "\",\"records\":";
    payload += recordJson.empty() ? std::string("null") : recordJson;
    payload += "}";

    Response r = sendRequest("insert", payload);
    return processResult(r.payload);
}

std::string Client::update(const std::string& path,
                           const std::string& recordJson,
                           const std::string& protopass) {
    PathParts p = parsePath(path, true);
    std::string payload;
    payload.reserve(128 + path.size() + recordJson.size());
    payload  = "{\"db\":\"";
    payload += simple_json_escape(p.db);
    payload += "\",\"table\":\"";
    payload += simple_json_escape(p.table);
    payload += "\",\"records\":";
    payload += recordJson.empty() ? std::string("null") : recordJson;
    payload += ",\"query\":\"\",\"protopass\":\"";
    payload += simple_json_escape(protopass);
    payload += "\",\"ids\":[\"";
    payload += simple_json_escape(p.id);
    payload += "\"]}";

    Response r = sendRequest("update", payload);
    return processResult(r.payload);
}

std::string Client::remove(const std::string& path,
                           const std::string& protopass) {
    PathParts p = parsePath(path, true);
    std::string payload;
    payload.reserve(128 + path.size());
    payload  = "{\"db\":\"";
    payload += simple_json_escape(p.db);
    payload += "\",\"table\":\"";
    payload += simple_json_escape(p.table);
    payload += "\",\"query\":\"\",\"protopass\":\"";
    payload += simple_json_escape(protopass);
    payload += "\",\"ids\":[\"";
    payload += simple_json_escape(p.id);
    payload += "\"]}";

    Response r = sendRequest("delete", payload);
    return processResult(r.payload);
}

std::string Client::onql(const std::string& query,
                          const std::string& protopass,
                          const std::string& ctxkey,
                          const std::string& ctxvaluesJson) {
    std::string payload;
    payload.reserve(96 + query.size() + ctxvaluesJson.size());
    payload  = "{\"query\":\"";
    payload += simple_json_escape(query);
    payload += "\",\"protopass\":\"";
    payload += simple_json_escape(protopass);
    payload += "\",\"ctxkey\":\"";
    payload += simple_json_escape(ctxkey);
    payload += "\",\"ctxvalues\":";
    payload += ctxvaluesJson.empty() ? std::string("[]") : ctxvaluesJson;
    payload += "}";

    Response r = sendRequest("onql", payload);
    return processResult(r.payload);
}

std::string Client::build(const std::string& query,
                          const std::vector<std::string>& values,
                          const std::vector<bool>& is_string) {
    std::string out = query;
    for (size_t i = 0; i < values.size(); ++i) {
        std::string placeholder = "$" + std::to_string(i + 1);
        bool quote = (i < is_string.size()) ? is_string[i] : false;
        std::string replacement = quote ? ("\"" + values[i] + "\"") : values[i];
        size_t pos = 0;
        while ((pos = out.find(placeholder, pos)) != std::string::npos) {
            out.replace(pos, placeholder.size(), replacement);
            pos += replacement.size();
        }
    }
    return out;
}

} // namespace onql
