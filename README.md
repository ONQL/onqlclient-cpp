# ONQL C++ Driver

Official C++ client library for the ONQL database server.

## Installation

The C++ driver is not currently published to vcpkg or Conan; install it from
source. Each tagged release also attaches prebuilt archives to its
[GitHub Release page](https://github.com/ONQL/onqlclient-cpp/releases) for
Linux, macOS, and Windows.

### Build from source

```bash
git clone https://github.com/ONQL/onqlclient-cpp.git
cd onqlclient-cpp
mkdir build && cd build
cmake ..
make
sudo make install
```

### CMake `FetchContent`

Consume directly from GitHub in your own project:

```cmake
include(FetchContent)
FetchContent_Declare(
    onql
    GIT_REPOSITORY https://github.com/ONQL/onqlclient-cpp.git
    GIT_TAG        v0.1.0   # or: main
)
FetchContent_MakeAvailable(onql)

target_link_libraries(your_target PRIVATE onql)
```

## Quick Start

```cpp
#include <iostream>
#include "onql/client.hpp"

int main() {
    auto client = onql::Client::connect("localhost", 5656);

    // Execute a query
    auto result = client.sendRequest("onql",
        R"({"db":"mydb","table":"users","query":"name = \"John\""})");
    std::cout << result.payload << std::endl;

    // Subscribe to live updates
    auto rid = client.subscribe("", R"(name = "John")",
        [](const std::string& rid, const std::string& keyword, const std::string& payload) {
            std::cout << "Update: " << payload << std::endl;
        });

    // Unsubscribe
    client.unsubscribe(rid);

    client.close();
    return 0;
}
```

## API Reference

### `onql::Client::connect(host, port, options)`

Creates and returns a connected client.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `host` | `std::string` | `"localhost"` | Server hostname |
| `port` | `int` | `5656` | Server port |
| `timeout` | `std::chrono::seconds` | `10s` | Default request timeout |

### `client.sendRequest(keyword, payload, timeout)`

Sends a request and waits for a response. Returns a `Response` struct.

### `client.subscribe(onquery, query, callback)`

Opens a streaming subscription. Returns the subscription ID.

### `client.unsubscribe(rid)`

Stops receiving events for a subscription.

### `client.close()`

Closes the connection.

## Direct ORM-style API

On top of raw `sendRequest`, the client exposes convenience methods that
build the standard payload envelopes for the common `insert` / `update` /
`delete` / `onql` operations and parse the `{error, data}` server response.

Because the driver is dependency-free, every JSON-valued parameter (records,
query, ids, ctxvalues) is passed as a **pre-serialized JSON string** — use
your favourite C++ JSON library (nlohmann/json, RapidJSON, …) to serialise.

Each helper returns the raw `data` substring of the server envelope, or
throws `std::runtime_error` if the server reports an error.

Call `client.setup(db)` once to bind a default database name; subsequent
`insert` / `update` / `remove` / `onql` calls will use it.

### `client.setup(db) -> Client&`

Sets the default database. Returns `*this` so calls can be chained.

```cpp
client.setup("mydb");
```

### `client.insert(table, recordsJson) -> std::string`

Insert one record or an array of records.

```cpp
client.insert("users", R"({"name":"John","age":30})");
client.insert("users", R"([{"name":"A"},{"name":"B"}])");
```

### `client.update(table, recordsJson, queryJson, protopass = "default", idsJson = "[]") -> std::string`

Update records matching `queryJson`.

```cpp
client.update("users", R"({"age":31})", R"({"name":"John"})");
client.update("users", R"({"active":false})", R"({"id":"u1"})", "admin");
```

### `client.remove(table, queryJson, protopass = "default", idsJson = "[]") -> std::string`

Delete records matching `queryJson`. Named `remove` to avoid clashing with
C++ reserved semantics around `delete`.

```cpp
client.remove("users", R"({"active":false})");
```

### `client.onql(query, protopass = "default", ctxkey = "", ctxvaluesJson = "[]") -> std::string`

Run a raw ONQL query. The server's `{error, data}` envelope is unwrapped.

```cpp
auto rows = client.onql("select * from users where age > 18");
```

### `onql::Client::build(query, values, is_string = {}) -> std::string`

Static helper that replaces `$1`, `$2`, … placeholders with values. When
`is_string[i]` is `true` the value is double-quoted; otherwise it is inlined
verbatim (suitable for numbers and booleans). `is_string` may be omitted or
shorter than `values` — any missing entry defaults to unquoted.

```cpp
auto q = onql::Client::build(
    "select * from users where name = $1 and age > $2",
    { "John", "18" },
    { true,   false }
);
// -> select * from users where name = "John" and age > 18
auto rows = client.onql(q);
```

### `onql::Client::processResult(raw) -> std::string`

Static helper that parses the standard `{error, data}` server envelope.
Throws `std::runtime_error` on non-empty `error`; returns the raw `data`
substring on success. Useful when you prefer to build payloads yourself.

### Full example

```cpp
#include <iostream>
#include "onql/client.hpp"

int main() {
    auto client = onql::Client::connect("localhost", 5656);
    client.setup("mydb");

    client.insert("users", R"({"name":"John","age":30})");

    auto q = onql::Client::build(
        "select * from users where age >= $1",
        { "18" }, { false });
    std::cout << client.onql(q) << std::endl;

    client.update("users", R"({"age":31})", R"({"name":"John"})");
    client.remove("users", R"({"name":"John"})");

    client.close();
    return 0;
}
```

## Protocol

The client communicates over TCP using a delimiter-based message format:

```
<request_id>\x1E<keyword>\x1E<payload>\x04
```

- `\x1E` — field delimiter
- `\x04` — end-of-message marker

## License

MIT
