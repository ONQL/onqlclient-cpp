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

    client.insert("mydb.users", R"({"id":"u1","name":"John","age":30})");

    auto rows = client.onql("select * from mydb.users where age > 18");
    std::cout << rows << std::endl;

    client.update("mydb.users.u1", R"({"age":31})");
    client.remove("mydb.users.u1");

    client.close();
    return 0;
}
```

## API Reference

### `onql::Client::connect(host, port, timeout)`

Creates and returns a connected client.

### `client.sendRequest(keyword, payload, timeout)`

Sends a raw request frame and waits for a response. Returns a `Response`
struct.

### `client.close()`

Closes the connection.

## Direct ORM-style API

On top of raw `sendRequest`, the client exposes convenience methods that
build the standard payload envelopes for the common `insert` / `update` /
`delete` / `onql` operations and parse the `{error, data}` server response.

Because the driver is dependency-free, every JSON-valued parameter (record,
ctxvalues) is passed as a **pre-serialized JSON string** — use your favourite
C++ JSON library (nlohmann/json, RapidJSON, …) to serialise.

The `path` argument is a **dotted string**:

| Path shape | Meaning |
|------------|---------|
| `"mydb.users"` | Table (used by `insert`) |
| `"mydb.users.u1"` | Record id `u1` (used by `update` / `remove`) |

Each helper returns the raw `data` substring of the server envelope, or
throws `std::runtime_error` if the server reports an error.

### `client.insert(path, recordJson) -> std::string`

Insert a **single** record.

```cpp
client.insert("mydb.users", R"({"id":"u1","name":"John","age":30})");
```

### `client.update(path, recordJson, protopass = "default") -> std::string`

Update the record at `path`.

```cpp
client.update("mydb.users.u1", R"({"age":31})");
client.update("mydb.users.u1", R"({"active":false})", "admin");
```

### `client.remove(path, protopass = "default") -> std::string`

Delete the record at `path`. Named `remove` to avoid clashing with the C++
keyword `delete`.

```cpp
client.remove("mydb.users.u1");
```

### `client.onql(query, protopass = "default", ctxkey = "", ctxvaluesJson = "[]") -> std::string`

Run a raw ONQL query.

```cpp
auto rows = client.onql("select * from mydb.users where age > 18");
```

### `onql::Client::build(query, values, is_string = {}) -> std::string`

Static helper that replaces `$1`, `$2`, … placeholders. When `is_string[i]`
is `true` the value is double-quoted; otherwise it is inlined verbatim.

```cpp
auto q = onql::Client::build(
    "select * from mydb.users where name = $1 and age > $2",
    { "John", "18" },
    { true,   false }
);
auto rows = client.onql(q);
```

### `onql::Client::processResult(raw) -> std::string`

Static helper that parses the standard `{error, data}` server envelope.

## Protocol

```
<request_id>\x1E<keyword>\x1E<payload>\x04
```

- `\x1E` — field delimiter
- `\x04` — end-of-message marker

## License

MIT
