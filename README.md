![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header Only](https://img.shields.io/badge/header--only-yes-brightgreen.svg)
![SIMD](https://img.shields.io/badge/SIMD-SSE2%20%7C%20AVX2-orange.svg)
![MIT License](https://img.shields.io/badge/license-MIT-green.svg)

## Overview

ujson is a modern C++20 JSON library providing:

- High-performance JSON parsing (DOM + SAX)
- JSON building / mutation APIs (DOM builder + flatten-style API)
- High-throughput encoding
- SIMD-accelerated structural scanning (SSE2 / AVX2)
- Explicit allocator control (PMR-style)
- Strong compile-time guarantees via C++20 Concepts
- Single-header integration

The design targets predictable memory behavior and minimal allocation overhead via arena-backed storage.

---

## Lightweight by Design

ujson is designed to minimize:

- Binary size
- Dynamic allocations
- Runtime overhead
- Dependency surface

The entire library is single-header, requires no external dependencies,
and uses arena-backed storage to keep allocation costs close to the theoretical minimum.

---

## Features

### Single Header

- Header-only integration
- No external runtime dependencies
- Drop-in usage

---

## Modern C++20 Design

ujson is written specifically for C++20 and uses:

- `constexpr` classification tables
- No RTTI
- No dynamic polymorphism in hot paths

### C++20 Concepts

Concepts are used to enforce compile-time contracts:

- SAX handler validation
- Allocator-like constraints
- Parser handler interface guarantees

Benefits:

- Clear compiler diagnostics
- Strong type safety
- No runtime contract checks
- Zero overhead abstraction

---

## Parsing APIs

### DOM Parser

- Arena-backed allocation
- Deterministic memory layout
- Fast structural index generation
- Depth-limited parsing

### SAX Parser

- Event-driven parsing
- No DOM materialization
- Compile-time validated handler interface

---

## Builder APIs

ujson includes a DOM builder designed around arena allocation and minimal overhead per node.

### ValueBuilder (DOM builder)

- Arena-backed node allocation
- Optional string materialization policy (copy/view)
- Object indexing for fast key lookup and mutation
- Insert/assign, erase, and container growth with predictable behavior

### Flatten-style API (NodeRef)

The builder exposes a convenient flatten-style mutation API through a lightweight reference type:

- `root()["a"]["b"][0] = 123`
- `root().add("key", value)` / `root().erase("key")`
- `root()[index]` auto-expands arrays (when configured) and returns a writable reference

This enables concise construction and mutation without exposing internal node ownership.

---

## SIMD Structural Scan

- SSE2 (128-bit)
- AVX2 (256-bit)
- Compile-time specialization (no runtime CPU dispatch)
- Branch-minimized structural detection

---

## Compile-Time Character Tables

- 256-entry constexpr bitmask tables
- O(1) classification
- Prefix-XOR masking for string detection
- No runtime table construction

---

## Custom Allocator Support

- PMR-style design
- Custom allocator injection
- Arena-backed DOM materialization
- No mandatory global `new/delete`

Designed for engines, memory-controlled systems, and deterministic environments.

---

## Allocation Behavior

ujson is designed to keep allocations close to the theoretical minimum:

- DOM parsing and building allocate primarily from the arena
- No per-node heap allocations
- Optional string materialization policy (view/copy) to control ownership

In typical workflows, allocations are limited to arena growth and string materialization when enabled.

---

## Unicode Support

- UTF-8
- UTF-16
- UTF-32
- Surrogate pairs
- `\u0000`
- Validation and decoding

---

## Benchmark

Environment:

CPU: Intel Core Ultra 7 255H  
RAM: 32 GB  
OS: Windows x64  
Compiler: MSVC 19.50 (/O2)  
SIMD: SSE2 / AVX2  
Dataset: Twitter JSON (617 KB, mmap)  
RapidJSON built with SSE4.2 enabled  

### AVX2 Build (Twitter 617 KB)

| Mode       | ujson | RapidJSON |
|------------|--------|------------|
| DOM Parse  | 1.29 ms | 1.31 ms |
| SAX Parse  | 0.90 ms | 1.04 ms |
| Encode     | 0.35 ms | 0.53 ms |

Approximate throughput:

| Mode      | ujson | RapidJSON |
|-----------|--------|------------|
| SAX Parse | ~670 MB/s | ~580 MB/s |
| Encode    | ~1.7 GB/s | ~1.1 GB/s |

---

## Usage

### Include

```cpp
#include "ujson.hpp"
```

---

### DOM Parsing

```cpp
ujson::NewAllocator alloc{};
ujson::Arena arena{alloc};

std::string_view json = R"({"key": 42})";

auto doc = ujson::Document::parse(json, arena);
if (!doc.ok()) {
    // handle parse error
}

auto root = doc.root();
auto value = root.get("key").as_i64();
```

---

### Building JSON (ValueBuilder)

```cpp
ujson::NewAllocator alloc{};
ujson::Arena arena{alloc};

ujson::ValueBuilder b{arena};

auto r = b.root();
r.set_object();

r.add("id", 42);
r.add("name", "ujson");

ujson::ParseError err{};
std::string encoded = ujson::encode(r, false, &err);
```

---

### Flatten-style Builder API (NodeRef)

```cpp
ujson::NewAllocator alloc{};
ujson::Arena arena{alloc};

ujson::ValueBuilder b{arena};
auto r = b.root();

r["user"]["id"] = 1337;
r["user"]["name"] = "alice";

r["items"][0] = 1;
r["items"][1] = 2;
r["items"][2] = 3;

r["meta"].set_object();
r["meta"].add("version", 1);

ujson::ParseError err{};
std::string encoded = ujson::encode(r, false, &err);
```

---

### SAX Parsing

```cpp
struct MyHandler {
    bool on_null() { return true; }
    bool on_bool(bool) { return true; }
    bool on_integer(std::int64_t) { return true; }
    bool on_number(double) { return true; }
    bool on_string(std::string_view) { return true; }
    bool on_start_object() { return true; }
    bool on_key(std::string_view) { return true; }
    bool on_end_object() { return true; }
    bool on_start_array() { return true; }
    bool on_end_array() { return true; }
};

MyHandler handler;
ujson::SaxParser parser(handler, json, 512);

auto err = parser.parse();
```

---

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### AVX2 Target

```bash
cmake --build build --target ujson_tests_avx2 --config Release
```

---

## Compiler Flags

MSVC:

```
/O2 /arch:AVX2
```

GCC/Clang:

```
-O3 -mavx2
```

---

## Design Principles

- SIMD only where measurable
- Deterministic allocator behavior
- Compile-time safety via Concepts
- Zero dynamic polymorphism in hot paths
- Modern C++20-first architecture
- Parser + Builder symmetry

---

## License

MIT License.

See LICENSE file for details.


---

# Usage Examples

## Document

```cpp
#include <ujson/ujson.hpp>

auto doc = ujson::Document::parse(R"({"x":1,"arr":[true,"ok"]})");
if (doc.ok()) {
    auto root = doc.root();
    auto x = root["x"].as_i64();
    auto ok = root["arr"][1].as_string();
}
```

---

## Validator

```cpp
auto err = ujson::validate(R"({"x":1})");
if (!err.ok()) {
    auto msg = err.format<ujson::ErrorFormat::Compact>();
}
```

---

## SaxParser

```cpp
struct Handler {
    ujson::Arena& arena();
    bool on_null();
    bool on_bool(bool);
    bool on_integer(std::int64_t);
    bool on_number(double);
    bool on_string(std::string_view);
    bool on_key(std::string_view);
    bool on_array_begin();
    bool on_array_end();
    bool on_object_begin();
    bool on_object_end();
};

Handler h{/*...*/};
ujson::SaxParser parser(h, R"({"x":1,"y":2})");
auto err = parser.parse();
```

---

## DomBuilder

```cpp
ujson::DomBuilder b;

auto* root = b.object([&] {
    b["name"] = "ujson";
    b["n"] = 3;
    b.array([&] { 
        b.value(true); 
        b.value(1); 
    });
});

auto json = b.encode(false);
```

---

## ValueBuilder

```cpp
ujson::ValueBuilder vb;

auto root = vb.root();
root.set_object();

root.add("name", "ujson");
root.add("n", 3);

auto arr = root.add_array("items");
arr.add(true);
arr.add(1);

auto json = vb.encode();
```

---

## Key format

```cpp
ujson::NewAllocator alloc{};
ujson::Arena arena{alloc};

arena.set_key_format(ujson::detail::snake_case);
auto fmt = arena.key_format();

ujson::ValueBuilder::Options opt{};
opt.key_format = fmt;

ujson::ValueBuilder vb{arena, opt};
auto root = vb.root().set_object();

root.add("fooBar", 1);
root.add("FooBaz", 2);

auto json = vb.encode();
```
