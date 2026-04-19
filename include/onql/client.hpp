/*
 * ONQL C++ Driver - Public API
 *
 * Thread-safe async TCP client for the ONQL database server.
 * Protocol: messages delimited by \x04, fields delimited by \x1E.
 * Message format: {request_id}\x1E{keyword}\x1E{payload}\x04
 */

#ifndef ONQL_CLIENT_HPP
#define ONQL_CLIENT_HPP

#include <string>
#include <functional>
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

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/** Response from a sendRequest() call. */
struct Response {
    std::string requestId;
    std::string source;
    std::string payload;
};

/** Callback type for subscriptions: (rid, source, payload). */
using SubscriptionCallback =
    std::function<void(const std::string&, const std::string&, const std::string&)>;

/* ------------------------------------------------------------------ */
/* Exceptions                                                          */
/* ------------------------------------------------------------------ */

class ConnectionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class TimeoutError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/* ------------------------------------------------------------------ */
/* Client                                                              */
/* ------------------------------------------------------------------ */

class Client {
public:
    /**
     * Connect to an ONQL server.
     *
     * @param host    Server hostname or IP.
     * @param port    Server port.
     * @param timeout Default request timeout.
     * @return        A connected Client instance.
     * @throws ConnectionError on failure.
     */
    static Client connect(const std::string& host,
                          int port,
                          std::chrono::seconds timeout = std::chrono::seconds(10));

    /** Move-only — no copies. */
    Client(Client&& other) noexcept;
    Client& operator=(Client&& other) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    ~Client();

    /**
     * Send a request and block until the response arrives or timeout elapses.
     *
     * @param keyword  The ONQL keyword (e.g. "onql").
     * @param payload  The request payload string.
     * @param timeout  Per-request timeout (0 = use default).
     * @return         The server response.
     * @throws TimeoutError      if the response doesn't arrive in time.
     * @throws ConnectionError   if the connection is lost.
     */
    Response sendRequest(const std::string& keyword,
                         const std::string& payload,
                         std::chrono::seconds timeout = std::chrono::seconds(0));

    /**
     * Open a streaming subscription.
     *
     * @param onquery   The onquery identifier.
     * @param query     The ONQL query.
     * @param callback  Invoked for each streamed frame (rid, source, payload).
     * @return          The subscription request ID.
     */
    std::string subscribe(const std::string& onquery,
                          const std::string& query,
                          SubscriptionCallback callback);

    /**
     * Cancel a subscription.
     *
     * @param rid  The subscription request ID returned by subscribe().
     */
    void unsubscribe(const std::string& rid);

    /** Close the connection and stop the background reader. */
    void close();

    // ------------------------------------------------------------------
    //  Direct ORM-style API (setup / insert / update / delete / onql /
    //  build / processResult)
    //
    //  These helpers build the standard ONQL payload envelopes for
    //  common operations and parse the `{error, data}` server response.
    //  Because the driver is dependency-free, every JSON-valued
    //  parameter (records, query, ids, ctxvalues) is passed as a
    //  pre-serialized JSON string; use your favourite C++ JSON library
    //  (nlohmann/json, RapidJSON, ...) to serialise.
    // ------------------------------------------------------------------

    /**
     * Set the default database name used by insert / update / delete /
     * onql. Returns `*this` so calls can be chained.
     */
    Client& setup(const std::string& db);

    /**
     * Insert one record or a list of records.
     *
     * @param table        Target table.
     * @param recordsJson  JSON object, or array of objects.
     * @return             The decoded `data` substring from the server
     *                     envelope.
     * @throws std::runtime_error on server-reported error.
     */
    std::string insert(const std::string& table,
                       const std::string& recordsJson);

    /**
     * Update records matching `queryJson`.
     *
     * @param table        Target table.
     * @param recordsJson  JSON object of fields to update.
     * @param queryJson    JSON query.
     * @param protopass    Proto-pass profile (default `"default"`).
     * @param idsJson      JSON array of explicit record IDs
     *                     (default `"[]"`).
     */
    std::string update(const std::string& table,
                       const std::string& recordsJson,
                       const std::string& queryJson,
                       const std::string& protopass = "default",
                       const std::string& idsJson   = "[]");

    /**
     * Delete records matching `queryJson`. Same parameter semantics as
     * `update`.
     */
    std::string remove(const std::string& table,
                       const std::string& queryJson,
                       const std::string& protopass = "default",
                       const std::string& idsJson   = "[]");

    /**
     * Execute a raw ONQL query.
     *
     * @param query          ONQL query text.
     * @param protopass      Proto-pass profile (default `"default"`).
     * @param ctxkey         Context key (default `""`).
     * @param ctxvaluesJson  JSON array of context values (default `"[]"`).
     */
    std::string onql(const std::string& query,
                     const std::string& protopass     = "default",
                     const std::string& ctxkey        = "",
                     const std::string& ctxvaluesJson = "[]");

    /**
     * Replace `$1`, `$2`, ... placeholders in `query` with the values.
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

    /* Platform socket handle */
#ifdef _WIN32
    using sock_t = unsigned long long;   /* SOCKET */
#else
    using sock_t = int;
#endif
    static constexpr auto SOCK_INVALID =
#ifdef _WIN32
        static_cast<sock_t>(~0ULL);
#else
        static_cast<sock_t>(-1);
#endif

    /* Internal helpers */
    void sendRaw(const std::string& data);
    std::string generateRequestId();
    void readerLoop();

    /* Per-pending-request slot */
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
    std::unordered_map<std::string, SubscriptionCallback>             subs_;

    std::string           db_;   /* default database for ORM-style API */
};

} // namespace onql

#endif // ONQL_CLIENT_HPP
