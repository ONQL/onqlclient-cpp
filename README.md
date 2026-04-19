# ONQL C++ Driver

Official C++ client library for the ONQL database server.

## Installation

The C++ driver is not currently published to vcpkg or Conan; install it from
source. Each tagged release also attaches prebuilt archives to its
[GitHub Release page](https://github.com/ONQL/onqlclient-cpp/releases).

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
    GIT_TAG        v0.1.0
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

    client.insert("mydb", "users",
        R"({"id":"u1","name":"John","age":30})");

    auto rows = client.onql("mydb.users[age>18]");
    std::cout << rows << std::endl;

    auto q = onql::Client::build("mydb.users[id=$1].id",
                                 { "u1" }, { true });
    client.update("mydb", "users", R"({"age":31})", q);

    client.remove("mydb", "users", "", "default", R"(["u1"])");

    client.close();
    return 0;
}
```

## API Reference

### `onql::Client::connect(host, port, timeout)`

Creates and returns a connected client.

### `client.sendRequest(keyword, payload, timeout)`

Sends a raw request frame and waits for a response.

### `client.close()`

Closes the connection.

## Direct ORM-style API

On top of raw `sendRequest`, the client exposes convenience methods that
build the standard payload envelopes for the common `insert` / `update` /
`remove` / `onql` operations and parse the `{error, data}` server response.

Because the driver is dependency-free, every JSON-valued parameter is passed
as a **pre-serialized JSON string** — use your favourite C++ JSON library.

`db` is passed explicitly to `insert` / `update` / `remove`. `onql` takes a
fully-qualified ONQL expression.

`query` arguments are **ONQL expression strings**, e.g.
`mydb.users[id="u1"].id`.

Each helper returns the raw `data` substring of the server envelope, or
throws `std::runtime_error` if the server reports an error.

### `client.insert(db, table, recordJson) -> std::string`

Insert a **single** record.

```cpp
client.insert("mydb", "users", R"({"id":"u1","name":"John","age":30})");
```

### `client.update(db, table, recordJson, query, protopass = "default", idsJson = "[]") -> std::string`

Update records matching `query` (or `idsJson`).

```cpp
client.update("mydb", "users", R"({"age":31})",
              R"(mydb.users[id="u1"].id)");

client.update("mydb", "users", R"({"active":false})",
              "", "admin", R"(["u1"])");
```

### `client.remove(db, table, query, protopass = "default", idsJson = "[]") -> std::string`

Delete records matching `query` (or `idsJson`). Named `remove` to avoid the
C++ keyword `delete`.

```cpp
client.remove("mydb", "users", R"(mydb.users[id="u1"].id)");
client.remove("mydb", "users", "", "default", R"(["u1"])");
```

### `client.onql(query, protopass = "default", ctxkey = "", ctxvaluesJson = "[]") -> std::string`

Run a raw ONQL query.

```cpp
auto rows = client.onql("mydb.users[age>18]");
```

### `onql::Client::build(query, values, is_string = {}) -> std::string`

Static helper that replaces `$1`, `$2`, … placeholders.

```cpp
auto q = onql::Client::build(
    "mydb.users[name=$1 and age>$2]",
    { "John", "18" },
    { true,   false }
);
auto rows = client.onql(q);
```

### `onql::Client::processResult(raw) -> std::string`

Static helper that parses the `{error, data}` envelope.

## Protocol

```
<request_id>\x1E<keyword>\x1E<payload>\x04
```

- `\x1E` — field delimiter
- `\x04` — end-of-message marker

## License

MIT
