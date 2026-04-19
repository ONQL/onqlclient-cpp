/*
 * ONQL C++ Driver - Public API
 */

#ifndef ONQL_CLIENT_HPP
#define ONQL_CLIENT_HPP

#include <string>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <memory>
#include <atomic>
#include <vector>

namespace onql {

/** Response from a sendRequest() call. */
struct Response {
    std::string requestId;
    std::string source;
    std::string payload;
};

class ConnectionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class TimeoutError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Client {
public:
    /**
     * Connect to an ONQL server.
     */
    static Client connect(const std::string& host,
                          int port,
                          std::chrono::seconds timeout = std::chrono::seconds(10));

    Client(Client&& other) noexcept;
    Client& operator=(Client&& other) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    ~Client();

    /** Send a raw request frame and block until the response arrives. */
    Response sendRequest(const std::string& keyword,
                         const std::string& payload,
                         std::chrono::seconds timeout = std::chrono::seconds(0));

    /** Close the connection and stop the background reader. */
    void close();

    // ------------------------------------------------------------------
    //  Direct ORM-style API (insert / update / remove / onql / build /
    //  processResult)
    //
    //  `query` arguments are ONQL expression strings, e.g.
    //    "mydb.users[id=\"u1\"].id"
    //    "mydb.orders[status=\"pending\"]"
    //
    //  Because the driver is dependency-free, every JSON-valued
    //  parameter (record, ids, ctxvalues) is passed as a pre-serialized
    //  JSON string; use your favourite C++ JSON library
    //  (nlohmann/json, RapidJSON, ...) to serialise.
    // ------------------------------------------------------------------

    /** Insert a single record into `db.table`. */
    std::string insert(const std::string& db,
                       const std::string& table,
                       const std::string& recordJson);

    /**
     * Update records in `db.table` matching `query` (or the explicit
     * `idsJson`). `query` is an ONQL expression; pass "" when using
     * `idsJson`. `idsJson` is a JSON array of record IDs, e.g.
     * `"[]"` or `"[\"u1\"]"`.
     */
    std::string update(const std::string& db,
                       const std::string& table,
                       const std::string& recordJson,
                       const std::string& query,
                       const std::string& protopass = "default",
                       const std::string& idsJson   = "[]");

    /**
     * Delete records in `db.table`. Named `remove` to avoid the C++
     * keyword `delete`.
     */
    std::string remove(const std::string& db,
                       const std::string& table,
                       const std::string& query,
                       const std::string& protopass = "default",
                       const std::string& idsJson   = "[]");

    /** Execute a raw ONQL query. */
    std::string onql(const std::string& query,
                     const std::string& protopass     = "default",
                     const std::string& ctxkey        = "",
                     const std::string& ctxvaluesJson = "[]");

    /**
     * Replace `$1`, `$2`, ... placeholders with values.
     * If `is_string[i]` is true, the value is double-quoted; otherwise
     * it is inlined verbatim.
     */
    static std::string build(const std::string& query,
                             const std::vector<std::string>& values,
                             const std::vector<bool>& is_string = {});

    /**
     * Parse the standard `{"error":"…","data":…}` envelope.
     * Returns the decoded `data` substring on success; throws
     * `std::runtime_error` with the error message otherwise.
     */
    static std::string processResult(const std::string& raw);

private:
    Client();

#ifdef _WIN32
    using sock_t = unsigned long long;
#else
    using sock_t = int;
#endif
    static constexpr auto SOCK_INVALID =
#ifdef _WIN32
        static_cast<sock_t>(~0ULL);
#else
        static_cast<sock_t>(-1);
#endif

    void sendRaw(const std::string& data);
    std::string generateRequestId();
    void readerLoop();

    struct PendingRequest {
        std::condition_variable cv;
        Response response;
        bool ready = false;
        bool error = false;
    };

    sock_t                sock_;
    std::chrono::seconds  defaultTimeout_;
    std::atomic<bool>     running_;
    std::thread           readerThread_;

    std::mutex            mu_;
    std::string           recvBuf_;
    std::unordered_map<std::string, std::shared_ptr<PendingRequest>> pending_;
};

} // namespace onql

#endif // ONQL_CLIENT_HPP
