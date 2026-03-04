#include "catch2/catch_approx.hpp"

#include <catch2/catch_test_macros.hpp>
#include <ujson/ujson.hpp>

#include <iostream>
#include <memory>
#include <memory_resource>
#include <random>
#include <unordered_set>
#include <utility>

using namespace ujson;

struct CountingAllocator {
    static constexpr auto kBlockSize = kDefaultBlockSize;

    std::size_t alloc_count = 0;
    std::size_t dealloc_count = 0;

    std::size_t bytes_allocated = 0;
    std::size_t bytes_live = 0;

    void* allocate(std::size_t sz, std::size_t align) {
        ++alloc_count;
        bytes_allocated += sz;
        bytes_live += sz;

        void* p = nullptr;

#if defined(_MSC_VER)
        p = _aligned_malloc(sz, align);
#else
        posix_memalign(&p, align, sz);
#endif
        return p;
    }

    void deallocate(void* p, const std::size_t sz, std::size_t) {
        ++dealloc_count;
        bytes_live -= sz;

#if defined(_MSC_VER)
        _aligned_free(p);
#else
        std::free(p);
#endif
    }

    void reset_stats() {
        alloc_count = 0;
        dealloc_count = 0;
        bytes_allocated = 0;
        bytes_live = 0;
    }

    ~CountingAllocator() {
        if (bytes_live != 0) {
            std::cout << "\n==== Allocator statistics ====\n";
            std::cout << "alloc calls     : " << alloc_count << "\n";
            std::cout << "dealloc calls   : " << dealloc_count << "\n";
            std::cout << "bytes allocated : " << bytes_allocated << "\n";
            std::cout << "bytes live      : " << bytes_live << "\n";

            if (bytes_live != 0) {
                std::cout << "MEMORY LEAK DETECTED: " << bytes_live << " bytes\n";
            } else {
                std::cout << "no leaks\n";
            }

            std::cout << "==============================\n";
        }
    }
};

struct UJsonArena {
    CountingAllocator alloc;
    ujson::Arena arena;
    UJsonArena(): alloc {}, arena {alloc} { }
};

using TestArena = ujson::Arena;
using TestDocument = DocumentView;

TEST_CASE("allocator actually allocates memory") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({"a":1,"b":2})"}, a.arena);
    REQUIRE(doc.ok());

    REQUIRE(a.alloc.alloc_count > 0);
    REQUIRE(a.alloc.bytes_allocated > 0);
}

TEST_CASE("allocator balanced allocations") {
    UJsonArena a;

    {
        auto doc = TestDocument::parse(std::string_view {R"({"a":1})"}, a.arena);
        REQUIRE(doc.ok());
    }

    a.arena.reset();

    REQUIRE(a.alloc.alloc_count == a.alloc.dealloc_count);
}

TEST_CASE("allocator stress memory growth") {
    UJsonArena a;

    for (auto i = 0; i < 100; ++i) {
        auto doc = TestDocument::parse(std::string_view {R"({"a":1,"b":[1,2,3]})"}, a.arena);
        REQUIRE(doc.ok());
        a.arena.reset();
    }

    a.arena.reset();
    REQUIRE(a.alloc.bytes_live == 0);
}

TEST_CASE("decode basic types", "[ujson][basic]") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({
        "b": true,
        "i": 42,
        "f": 3.5,
        "f": 3.5,
        "s": "hello",
        "n": null,
        "k": ""
    })"},
                                   a.arena);

    REQUIRE(doc.ok());

    auto root = doc.root();
    REQUIRE(root.is_object());

    CHECK(root.get("b").as_bool() == true);
    CHECK(static_cast<int>(root.get("i").as_double()) == 42);
    CHECK(root.get("f").as_double() == Catch::Approx(3.5));
    CHECK(root.get("s").as_string() == "hello");
    CHECK(root.get("").as_string().empty());

    REQUIRE(root.get("k").as_string().empty());
}

TEST_CASE("parse arrays", "[ujson]") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({
        "arr": [1, 2, 3, 4]
    })"},
                                   a.arena);

    REQUIRE(doc.ok());

    auto arr = doc.root().get("arr");
    REQUIRE(arr.is_array());
    REQUIRE(arr.size() == 4);

    int values[4] {};
    REQUIRE(arr.get_ints({}, values, 4));

    CHECK(values[0] == 1);
    CHECK(values[1] == 2);
    CHECK(values[2] == 3);
    CHECK(values[3] == 4);
}

TEST_CASE("nested objects", "[ujson]") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({
        "player": {
            "health": 100,
            "alive": true
        }
    })"},
                                   a.arena);

    REQUIRE(doc.ok());

    auto player = doc.root().get("player");
    REQUIRE(player.is_object());

    CHECK(static_cast<int>(player.get("health").as_double()) == 100);
    CHECK(player.get("alive").as_bool() == true);
}

TEST_CASE("builder encode/decode roundtrip", "[ujson]") {
    UJsonArena a;
    DomBuilder b {a.arena};

    [[maybe_unused]] Node* root = b.object([&] {
        b["a"] = 10;
        b["b"] = true;
        b["c"] = std::string_view {"test"};
    });

    std::string encoded = b.encode();

    auto doc = TestDocument::parse(encoded, a.arena);
    REQUIRE(doc.ok());

    auto r = doc.root();
    CHECK(static_cast<int>(r.get("a").as_double()) == 10);
    CHECK(r.get("b").as_bool() == true);
    CHECK(r.get("c").as_string() == "test");
}

TEST_CASE("validation fails on invalid json", "[ujson]") {
    CHECK_FALSE(ujson::validate(std::string_view {"{"}).ok());
    CHECK_FALSE(ujson::validate(std::string_view {R"({"a":,})"}).ok());
    CHECK_FALSE(ujson::validate(std::string_view {R"(["a" "b"])"}).ok());
}

TEST_CASE("utf8 strings", "[ujson]") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({
        "en": "Hello",
        "ru": "Привет",
        "cn": "你好"
    })"},
                                   a.arena);

    REQUIRE(doc.ok());

    CHECK(doc.root().get("en").as_string() == "Hello");
    CHECK(doc.root().get("ru").as_string() == "Привет");
    CHECK(doc.root().get("cn").as_string() == "你好");
}

TEST_CASE("depth limit", "[ujson]") {
    UJsonArena a;

    std::string deep = "[[[[[[[[[[1]]]]]]]]]]";

    auto doc = TestDocument::parse(deep, a.arena, 5);

    REQUIRE_FALSE(doc.ok());
    REQUIRE(doc.error().code == ErrorCode::DepthExceeded);
}

TEST_CASE("float and double precision", "[ujson]") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({
        "f1": 0.5,
        "f2": 3.1415926,
        "f3": -12.75
    })"},
                                   a.arena);

    REQUIRE(doc.ok());

    CHECK(doc.root().get("f1").as_double() == Catch::Approx(0.5));
    CHECK(doc.root().get("f2").as_double() == Catch::Approx(3.1415926));
    CHECK(doc.root().get("f3").as_double() == Catch::Approx(-12.75));
}

TEST_CASE("array of objects", "[ujson]") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({
        "players": [
            { "id": 1, "alive": true },
            { "id": 2, "alive": false },
            { "id": 3, "alive": true }
        ]
    })"},
                                   a.arena);

    REQUIRE(doc.ok());

    auto players = doc.root().get("players");
    REQUIRE(players.is_array());
    REQUIRE(players.size() == 3);

    int ids[3] {};
    bool alive[3] {};

    auto i = 0;
    players.for_each([&](const ValueRef v) {
        ids[i] = static_cast<int>(v.get("id").as_double());
        alive[i] = v.get("alive").as_bool();
        ++i;
    });

    CHECK(ids[0] == 1);
    CHECK(ids[1] == 2);
    CHECK(ids[2] == 3);

    CHECK(alive[0] == true);
    CHECK(alive[1] == false);
    CHECK(alive[2] == true);
}

TEST_CASE("nested arrays", "[ujson]") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({
        "grid": [
            [1,2,3],
            [4,5,6],
            [7,8,9]
        ]
    })"},
                                   a.arena);

    REQUIRE(doc.ok());

    auto grid = doc.root().get("grid");
    REQUIRE(grid.is_array());
    REQUIRE(grid.size() == 3);

    auto expected = 1;

    grid.for_each([&](const ValueRef row) {
        REQUIRE(row.is_array());
        row.for_each([&](const ValueRef cell) {
            REQUIRE(cell.is_number());
            CHECK(static_cast<int>(cell.as_double()) == expected++);
        });
    });
}

TEST_CASE("mixed array types", "[ujson]") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({
        "mix": [1, true, "str", null, { "x": 5 }]
    })"},
                                   a.arena);

    REQUIRE(doc.ok());

    auto mix = doc.root().get("mix");
    REQUIRE(mix.is_array());
    REQUIRE(mix.size() == 5);

    auto idx = 0;
    mix.for_each([&](const ValueRef e) {
        switch (idx++) {
        case 0:
            CHECK(e.is_number());
            break;
        case 1:
            CHECK(e.is_bool());
            break;
        case 2:
            CHECK(e.is_string());
            break;
        case 3:
            CHECK(e.is_null());
            break;
        case 4:
            CHECK(e.is_object());
            break;
        }
    });
}

TEST_CASE("parser invalid numbers", "[ujson]") {
    UJsonArena a;

    CHECK_FALSE(TestDocument::parse(std::string_view {R"({"x": -})"}, a.arena).ok());
    CHECK_FALSE(TestDocument::parse(std::string_view {R"({"x": 01})"}, a.arena).ok());
    CHECK_FALSE(TestDocument::parse(std::string_view {R"({"x": 1.})"}, a.arena).ok());
    CHECK_FALSE(TestDocument::parse(std::string_view {R"({"x": 1e})"}, a.arena).ok());
    CHECK_FALSE(TestDocument::parse(std::string_view {R"({"x": 1e+})"}, a.arena).ok());
}

TEST_CASE("trailing garbage", "[ujson]") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({"a":1} xxx)"}, a.arena);

    REQUIRE_FALSE(doc.ok());
    REQUIRE(doc.error().code == ErrorCode::TrailingGarbage);
}

TEST_CASE("string escape handling", "[ujson]") {
    UJsonArena a;

    auto doc = TDocument<false, true>::parse(std::string_view {R"({"s":"line\n\t\"quoted\"\\slash"})"}, a.arena);

    REQUIRE(doc.ok());

    auto s = doc.root().get("s").as_string();
    CHECK(s.find('\n') != std::string::npos);
    CHECK(s.find('\t') != std::string::npos);
    CHECK(s.find('"') != std::string::npos);
}

TEST_CASE("unicode surrogate pairs", "[ujson]") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({"emoji":"\uD83D\uDE03"})"}, a.arena);

    REQUIRE(doc.ok());

    auto s = doc.root().get("emoji").as_string();
    REQUIRE(!s.empty());
}

TEST_CASE("invalid surrogate rejected", "[ujson]") {
    UJsonArena a;

    CHECK_FALSE(TestDocument::parse(std::string_view {R"({"bad":"\uD800"})"}, a.arena).ok());
}

TEST_CASE("large integer fallback to double", "[ujson]") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({"big": 92233720368547758070})"}, a.arena);

    REQUIRE(doc.ok());

    auto v = doc.root().get("big").as_double();
    CHECK(v > 9e18);
}

TEST_CASE("encoder depth limit", "[ujson]") {
    UJsonArena a;
    DomBuilder b {a.arena};

    Node* root = b.array([&](DomBuilder& obj) {
        auto build = [&](auto&& self, const int depth) -> void {
            if (depth == 0)
                return;
            obj.array([&] { self(self, depth - 1); });
        };

        build(build, 2000);
    });

    ParseError err;
    JsonWriterCore<StringSink> encoder(StringSink {}, false, 100, &err);
    (void)encoder.write(ValueRef {root, &a.arena});
    REQUIRE_FALSE(err.ok());
}

TEST_CASE("validator edge cases", "[ujson]") {
    CHECK_FALSE(ujson::validate(std::string_view {R"({"a": tru})"}).ok());
    CHECK_FALSE(ujson::validate(std::string_view {R"({"a": 1e-})"}).ok());
    CHECK_FALSE(ujson::validate(std::string_view {R"(["a",])"}).ok());
    CHECK_FALSE(ujson::validate(std::string_view {R"({"a":1,})"}).ok());
}

TEST_CASE("object index large", "[ujson]") {
    UJsonArena a;

    std::string json = "{";
    for (auto i = 0; i < 30; ++i) {
        json += "\"k" + std::to_string(i) + "\":" + std::to_string(i);
        if (i != 29)
            json += ",";
    }
    json += "}";

    auto doc = TestDocument::parse(json, a.arena);
    REQUIRE(doc.ok());

    auto root = doc.root();

    for (auto i = 0; i < 30; ++i) {
        auto v = root.get("k" + std::to_string(i));
        REQUIRE(v.is_number());
        CHECK(static_cast<int>(v.as_double()) == i);
    }
}

TEST_CASE("roundtrip complex nested", "[ujson]") {
    UJsonArena a;

    auto doc = TestDocument::parse(std::string_view {R"({
        "arr":[1,2,{"x":[3,4]}],
        "str":"abc",
        "b":false
    })"},
                                   a.arena);

    REQUIRE(doc.ok());

    std::string encoded = encode(doc.root());

    auto doc2 = TestDocument::parse(encoded, a.arena);
    REQUIRE(doc2.ok());

    CHECK(doc2.root().get("str").as_string() == "abc");
    CHECK(doc2.root().get("b").as_bool() == false);
}

TEST_CASE("builder complex nested manual build", "[ujson][builder]") {
    UJsonArena a;
    DomBuilder b {a.arena};

    Node* root = b.object([&] {
        (void)b["data"];
        b.array([&] {
            b.value(1);
            b.value(2);
            b.object([&] {
                b["flag"] = true;
                b["status"] = std::string_view {"ok"};
            });
        });

        b["type"] = std::string_view {"manual"};
    });

    std::string encoded = b.encode();

    auto doc = TestDocument::parse(encoded, a.arena);
    REQUIRE(doc.ok());

    auto r = doc.root();
    REQUIRE(r.get("type").as_string() == "manual");

    auto data = r.get("data");
    REQUIRE(data.is_array());
    REQUIRE(data.size() == 3);

    CHECK(static_cast<int>(data.at(0).as_double()) == 1);
    CHECK(static_cast<int>(data.at(1).as_double()) == 2);

    auto obj = data.at(2);
    REQUIRE(obj.is_object());
    CHECK(obj.get("flag").as_bool() == true);
    CHECK(obj.get("status").as_string() == "ok");
}

TEST_CASE("builder large array manual", "[ujson][builder]") {
    UJsonArena a;
    DomBuilder b {a.arena};

    Node* root = b.array([&] {
        for (auto i = 0; i < 1000; ++i)
            b.value(i);
    });

    REQUIRE(root->data.kids.count == 1000);

    std::string encoded = b.encode();
    auto doc = TestDocument::parse(encoded, a.arena);

    REQUIRE(doc.ok());
    REQUIRE(doc.root().size() == 1000);
}

TEST_CASE("builder deep object tree", "[ujson][builder]") {
    UJsonArena a;
    DomBuilder b {a.arena};

    Node* root = b.object([&] {
        auto build = [&](auto&& self, const int depth) -> void {
            if (depth == 0)
                return;

            (void)b["child"];
            b.object([&] { self(self, depth - 1); });
        };

        build(build, 10);
    });

    std::string encoded = b.encode();
    auto doc = TestDocument::parse(encoded, a.arena);

    REQUIRE(doc.ok());

    auto v = doc.root();
    for (auto i = 0; i < 10; ++i)
        v = v.get("child");

    REQUIRE(v.is_object());
}

TEST_CASE("builder mixed types manual", "[ujson][builder]") {
    UJsonArena a;
    DomBuilder b {a.arena};

    Node* root = b.array([&] {
        b.value(nullptr);
        b.value(true);
        b.value(std::string_view {"str"});
        b.value(3.14);
    });

    std::string encoded = b.encode();
    auto doc = TestDocument::parse(encoded, a.arena);

    REQUIRE(doc.ok());

    auto arr = doc.root();
    REQUIRE(arr.size() == 4);

    CHECK(arr.at(0).is_null());
    CHECK(arr.at(1).is_bool());
    CHECK(arr.at(2).is_string());
    CHECK(arr.at(3).is_number());
}

TEST_CASE("builder duplicate keys allowed", "[ujson][builder]") {
    UJsonArena a;
    DomBuilder b {a.arena};

    Node* root = b.object([&] {
        b["x"] = 1;
        b["x"] = 2;
    });

    std::string encoded = b.encode();
    auto doc = TestDocument::parse(encoded, a.arena);

    REQUIRE(doc.ok());

    // Hash index should return first match (current impl behavior)
    auto v = doc.root().get("x");
    REQUIRE(v.is_number());
}

TEST_CASE("builder encode without reparse", "[ujson][builder]") {
    UJsonArena a;
    DomBuilder b {a.arena};

    b.object([&] { b["k"] = std::string_view {"test"}; });

    std::string encoded = b.encode();

    REQUIRE(encoded == R"({"k":"test"})");
}

TEST_CASE("allocator: NewAllocator basic parse/encode", "[ujson][alloc]") {
    using Alloc = NewAllocator;
    Alloc alloc {};
    Arena arena {alloc};

    auto doc = TestDocument::parse(std::string_view {R"({"a":1,"b":[true,false],"c":"test"})"}, arena);

    REQUIRE(doc.ok());

    auto root = doc.root();
    CHECK(root.get("a").as_double() == 1.0);
    CHECK(root.get("c").as_string() == "test");

    std::string out = encode(root);
    REQUIRE_FALSE(out.empty());
}

TEST_CASE("allocator: StaticBufferAllocator small json", "[ujson][alloc]") {
    using Alloc = ujson::StaticBufferAllocator<1024, 1000>;
    Alloc alloc {};
    ujson::Arena arena {alloc};

    auto doc = TestDocument::parse(std::string_view {R"({"x":42,"y":"hello"})"}, arena);

    REQUIRE(doc.ok());
    CHECK(doc.root().get("x").as_double() == 42);
}

TEST_CASE("allocator: StaticBufferAllocator overflow", "[ujson][alloc]") {
    using Alloc = ujson::StaticBufferAllocator<256>;
    Alloc alloc {};
    ujson::Arena arena {alloc};

    std::string big = "{";
    for (auto i = 0; i < 100; ++i)
        big += "\"k" + std::to_string(i) + "\":" + std::to_string(i) + ",";

    big += "\"z\":0}";

    const auto doc = TestDocument::parse(big, arena);
    REQUIRE(doc.error().code == ujson::ErrorCode::WriterOverflow);
}

TEST_CASE("allocator: PmrAllocator basic", "[ujson][alloc]") {
    std::byte buffer[64 * 1024];
    std::pmr::monotonic_buffer_resource resource(buffer, sizeof(buffer));

    ujson::PmrAllocator alloc {&resource};
    ujson::Arena arena {alloc};

    auto doc = TestDocument::parse(std::string_view {R"({"a":123,"b":"pmr"})"}, arena);

    REQUIRE(doc.ok());
    CHECK(doc.root().get("a").as_double() == 123);
}

TEST_CASE("allocator: deep nesting works", "[ujson][alloc]") {
    using Alloc = ujson::NewAllocator;
    Alloc alloc {};
    ujson::Arena arena {alloc};

    std::string deep = "[[[[[[[[[[1]]]]]]]]]]";

    auto doc = TestDocument::parse(deep, arena, 20);

    REQUIRE(doc.ok());
}

TEST_CASE("allocator: writer fixed buffer", "[ujson][alloc]") {
    using Alloc = ujson::NewAllocator;
    Alloc alloc {};
    ujson::Arena arena {alloc};

    auto doc = TestDocument::parse(std::string_view {R"({"a":1})"}, arena);

    REQUIRE(doc.ok());

    ujson::FixedWriter writer(arena, 256);
    if (!writer.write(doc.root())) {
        REQUIRE(false); // should not fail
    }

    auto view = writer.finish();

    REQUIRE(view == R"({"a":1})");
}

TEST_CASE("allocator: stress test many objects", "[ujson][alloc][stress]") {
    using Alloc = ujson::NewAllocator;
    Alloc alloc {};
    ujson::Arena arena {alloc};

    std::string json = "{";
    for (auto i = 0; i < 1000; ++i) {
        json += "\"k" + std::to_string(i) + "\":" + std::to_string(i);
        if (i != 999)
            json += ",";
    }
    json += "}";

    auto doc = TestDocument::parse(json, arena);
    REQUIRE(doc.ok());
}

TEST_CASE("allocator: alignment respected", "[ujson][alloc]") {
    using Alloc = ujson::NewAllocator;
    Alloc alloc {};
    ujson::Arena arena {alloc};

    void* p = arena.alloc(32, 32);
    REQUIRE(reinterpret_cast<std::uintptr_t>(p) % 32 == 0);
}

#ifndef UJSON_VALIDATOR_STRICT_UTF8
    #define UJSON_VALIDATOR_STRICT_UTF8 0
#endif

static bool dom_ok(std::string_view s) {
    UJsonArena a;
    const auto doc = TestDocument::parse(s, a.arena);
    /* if (!doc.ok()) {
        std::cout << doc.error().format<ErrorFormat::Compact>() << "\n";
    }*/
    return doc.ok();
}

static bool val_ok(std::string_view s) {
    const auto e = ujson::validate < UJSON_VALIDATOR_STRICT_UTF8 != 0 > (s);
    /*  if (!e.ok()) {
        std::cout << e.format<ErrorFormat::Compact>() << "\n";
    }*/
    return e.ok();
}

static void require_val_eq_dom(std::string_view s) {
    const bool v = val_ok(s);
    const bool d = dom_ok(s);
    INFO("json: " << std::string(s));
    REQUIRE(v == d);
}

TEST_CASE("validator: basic valid samples", "[ujson][validator]") {
    REQUIRE(val_ok(R"({})"));
    REQUIRE(val_ok(R"([])"));
    REQUIRE(val_ok(R"({"a":1,"b":true,"c":null,"d":"x"})"));
    REQUIRE(val_ok(R"([1,2,3,{"x":[false,null]}])"));
    REQUIRE(val_ok(R"({"s":"line\n\t\"q\"\\/"})"));
}

TEST_CASE("validator: basic invalid samples", "[ujson][validator]") {
    REQUIRE_FALSE(val_ok(R"({)"));
    REQUIRE_FALSE(val_ok(R"([)"));
    REQUIRE_FALSE(val_ok(R"({"a":,})"));
    REQUIRE_FALSE(val_ok(R"({"a":1,})"));
    REQUIRE_FALSE(val_ok(R"(["a" "b"])"));
    REQUIRE_FALSE(val_ok(R"(tru)"));
    REQUIRE_FALSE(val_ok(R"(nul)"));
}

TEST_CASE("validator: numbers edge cases", "[ujson][validator]") {
    REQUIRE_FALSE(val_ok(R"(01)"));
    REQUIRE_FALSE(val_ok(R"(-)"));
    REQUIRE_FALSE(val_ok(R"(1.)"));
    REQUIRE_FALSE(val_ok(R"(1e)"));
    REQUIRE_FALSE(val_ok(R"(1e+)"));
    REQUIRE_FALSE(val_ok(R"(-01)"));
    REQUIRE_FALSE(val_ok(R"(+1)"));

    REQUIRE(val_ok(R"(0)"));
    REQUIRE(val_ok(R"(-0)"));
    REQUIRE(val_ok(R"(10)"));
    REQUIRE(val_ok(R"(0.0)"));
    REQUIRE(val_ok(R"(1.25)"));
    REQUIRE(val_ok(R"(1e10)"));
    REQUIRE(val_ok(R"(1E-10)"));
    REQUIRE(val_ok(R"(-12.75e+2)"));
}

TEST_CASE("validator: strings - invalid control chars must fail", "[ujson][validator]") {
    // raw newline inside JSON string is invalid
    REQUIRE_FALSE(val_ok("{\"s\":\"a\nb\"}"));
    REQUIRE_FALSE(val_ok("{\"s\":\"a\rb\"}"));
    REQUIRE_FALSE(val_ok("{\"s\":\"a\tb\"}")); // tab raw

    // 0x1F raw control (unit separator) inside string
    std::string j = "{\"s\":\"x";
    j.push_back(char(0x1F));
    j += "y\"}";
    REQUIRE_FALSE(val_ok(j));
}

TEST_CASE("validator: strings - escape sequences", "[ujson][validator]") {
    // valid escapes
    REQUIRE(val_ok(R"({"s":"\" \\ \/ \b \f \n \r \t"})"));
    REQUIRE(val_ok(R"({"s":"\u0000"})"));
    REQUIRE(val_ok(R"({"s":"\u0041"})")); // 'A'

    // invalid escapes
    REQUIRE_FALSE(val_ok(R"({"s":"\a"})"));
    REQUIRE_FALSE(val_ok(R"({"s":"\x41"})"));
    REQUIRE_FALSE(val_ok(R"({"s":"\"})")); // string ends after backslash
    REQUIRE_FALSE(val_ok(R"({"s":"\u"})"));
    REQUIRE_FALSE(val_ok(R"({"s":"\u0"})"));
    REQUIRE_FALSE(val_ok(R"({"s":"\u00"})"));
    REQUIRE_FALSE(val_ok(R"({"s":"\u000"})"));
    REQUIRE_FALSE(val_ok(R"({"s":"\u00G0"})"));
    REQUIRE_FALSE(val_ok(R"({"s":"\uD83D"})")); // lone high surrogate
}

TEST_CASE("validator: unicode surrogate pairs valid/invalid", "[ujson][validator]") {
    // valid surrogate pair (smile)
    REQUIRE(val_ok(R"({"e":"\uD83D\uDE03"})"));

    // invalid ordering
    REQUIRE_FALSE(val_ok(R"({"e":"\uDE03\uD83D"})")); // low then high

    // low surrogate alone
    REQUIRE_FALSE(val_ok(R"({"e":"\uDE03"})"));

    // high surrogate not followed by \uXXXX
    REQUIRE_FALSE(val_ok(R"({"e":"\uD83D!"})"));

    // high surrogate followed by non-low
    REQUIRE_FALSE(val_ok(R"({"e":"\uD83D\u0041"})"));
}

TEST_CASE("validator: structural corner cases", "[ujson][validator]") {
    REQUIRE_FALSE(val_ok(R"({"a":1 "b":2})"));
    REQUIRE_FALSE(val_ok(R"({"a" : 1, "b" : 2,) )"));
    REQUIRE_FALSE(val_ok(R"([,1])"));
    REQUIRE_FALSE(val_ok(R"([1,])"));

    REQUIRE(val_ok(R"({"":0})")); // empty key ok
    REQUIRE(val_ok(R"({"a":{}})"));
    REQUIRE(val_ok(R"({"a":[]})"));
}

TEST_CASE("validator: trailing garbage", "[ujson][validator]") {
    REQUIRE_FALSE(val_ok(R"({"a":1} xxx)"));
    REQUIRE_FALSE(val_ok(R"(true false)"));
    REQUIRE_FALSE(val_ok(R"(0 1)"));
}

TEST_CASE("validator: whitespace handling", "[ujson][validator]") {
    REQUIRE(val_ok(" \n\t { \r\n \"a\" \t:\n 1 \r } \t "));
    REQUIRE(val_ok(" [ \n 1 , 2 , 3 \t ] "));
}

TEST_CASE("validator vs dom: equivalence on curated corpus", "[ujson][validator]") {
    constexpr std::string_view corpus[] = {
        R"({})",
        R"([])",
        R"({"a":1})",
        R"({"a":-0,"b":0.0,"c":1e10,"d":-12.75e+2})",
        R"({"s":"hello"})",
        R"({"s":"line\n\t\"q\"\\/"})",
        R"({"u":"\u0041\u0042\u0043"})",
        R"({"e":"\uD83D\uDE03"})",
        R"([1,true,false,null,"x",{"k":[1,2,3]}])",
        R"({"o":{"x":{"y":{"z":[1,2,3]}}}})",
        // invalid
        R"({)",
        R"({"a":,})",
        R"({"s":"\u00G0"})",
        R"({"e":"\uD83D"})",
        R"({"s":"a
b"})",
        R"([1,])",
        R"({"a":1} xxx)",
        R"(01)",
    };

    for (auto s : corpus)
        require_val_eq_dom(s);
}

TEST_CASE("validator: fuzz-ish string escape patterns (no crashes, matches DOM)", "[ujson][validator]") {

    std::vector<std::string> parts = {"a", "\\\\", "\\\"", "\\/", "\\b", "\\f", "\\n", "\\r", "\\t", "\\u0000", "\\u0041", "\\u00FF", "\\uD83D\\uDE03"};

    for (auto& p1 : parts) {
        for (auto& p2 : parts) {
            std::string j = "{\"s\":\"";
            j += p1;
            j += p2;
            j += "\"}";
            require_val_eq_dom(j);
        }
    }
}

using namespace ujson;

static constexpr std::size_t kLargeN = 50000;
static constexpr std::size_t kMediumN = 10000;

namespace {
    template <class Policy>
    void check_key_policy_builder(std::string_view input1, std::string_view input2, std::string_view expected1, std::string_view expected2) {
        TValueBuilder<Policy> b;
        auto root = b.root().set_object();

        root.add(input1, 1);
        root.add(input2, 2);

        REQUIRE(root.get(expected1).raw()->data.num.as_i64() == 1);
        REQUIRE(root.get(expected2).raw()->data.num.as_i64() == 2);
        REQUIRE(root.get(input1).raw()->data.num.as_i64() == 1);
        REQUIRE(root.get(input2).raw()->data.num.as_i64() == 2);

        const std::string encoded = b.encode();
        REQUIRE(encoded.find(expected1) != std::string::npos);
        REQUIRE(encoded.find(expected2) != std::string::npos);
        REQUIRE(encoded.find(input1) == std::string::npos);
        REQUIRE(encoded.find(input2) == std::string::npos);
    }

    template <class Policy>
    void check_key_policy_roundtrip(std::string_view input1, std::string_view input2, std::string_view expected1, std::string_view expected2) {
        UJsonArena a;
        TDomBuilder<Policy> b {a.arena};

        b.object([&] {
            b[input1] = 1;
            b[input2] = 2;
        });

        const std::string encoded = b.encode();
        REQUIRE(encoded.find(expected1) != std::string::npos);
        REQUIRE(encoded.find(expected2) != std::string::npos);
        REQUIRE(encoded.find(input1) == std::string::npos);
        REQUIRE(encoded.find(input2) == std::string::npos);

        auto doc = TestDocument::parse(encoded, a.arena);
        REQUIRE(doc.ok());
        REQUIRE(doc.root().get(expected1).as_i64() == 1);
        REQUIRE(doc.root().get(expected2).as_i64() == 2);
        REQUIRE_FALSE(doc.root().contains(input1));
        REQUIRE_FALSE(doc.root().contains(input2));
    }
} // namespace

TEST_CASE("Basic object insert and lookup", "[ujson][value_builder][basic]") {
    ValueBuilder b;
    auto root = b.root().set_object();

    root.add("a", 1);
    root.add("b", 2);
    root.add("c", 3);

    REQUIRE(root.contains("a"));
    REQUIRE(root.contains("b"));
    REQUIRE(root.contains("c"));

    REQUIRE(root.get("a").raw()->data.num.i == 1);
    REQUIRE(root.get("b").raw()->data.num.i == 2);
    REQUIRE(root.get("c").raw()->data.num.i == 3);

    REQUIRE(b.ok());
}

TEST_CASE("Key policy normalizes builder keys", "[ujson][value_builder][key_policy]") {
    SECTION("snake_case") {
        check_key_policy_builder<detail::key_policy_snake_case>("fooBar", "FooBaz", "foo_bar", "foo_baz");
    }

    SECTION("camel_case") {
        check_key_policy_builder<detail::key_policy_camel_case>("foo_bar", "foo-baz", "fooBar", "fooBaz");
    }

    SECTION("pascal_case") {
        check_key_policy_builder<detail::key_policy_pascal_case>("foo_bar", "foo-baz", "FooBar", "FooBaz");
    }
}

TEST_CASE("Key policy writer/reader roundtrip", "[ujson][builder][key_policy]") {
    SECTION("snake_case") {
        check_key_policy_roundtrip<detail::key_policy_snake_case>("fooBar", "FooBaz", "foo_bar", "foo_baz");
    }

    SECTION("camel_case") {
        check_key_policy_roundtrip<detail::key_policy_camel_case>("foo_bar", "foo-baz", "fooBar", "fooBaz");
    }

    SECTION("pascal_case") {
        check_key_policy_roundtrip<detail::key_policy_pascal_case>("foo_bar", "foo-baz", "FooBar", "FooBaz");
    }
}

TEST_CASE("Key policy normalizes parsed keys", "[ujson][parser][key_policy]") {
    SECTION("snake_case") {
        const std::string json = R"({"fooBar":1,"FooBaz":2})";

        using SnakeDocument = TDocument<false, false, true, detail::key_policy_snake_case>;
        auto doc = SnakeDocument::parse(json);

        REQUIRE(doc.ok());
        REQUIRE(doc.root().get("foo_bar").as_i64() == 1);
        REQUIRE(doc.root().get("foo_baz").as_i64() == 2);
        REQUIRE(doc.root().contains("FooBaz"));
    }

    SECTION("camel_case") {
        const std::string json = R"({"foo_bar":1,"foo-baz":2})";

        using CamelDocument = TDocument<false, false, true, detail::key_policy_camel_case>;
        auto doc = CamelDocument::parse(json);

        REQUIRE(doc.ok());
        REQUIRE(doc.root().get("fooBar").as_i64() == 1);
        REQUIRE(doc.root().get("fooBaz").as_i64() == 2);
        REQUIRE(doc.root().contains("foo_bar"));
    }

    SECTION("pascal_case") {
        const std::string json = R"({"foo_bar":1,"foo-baz":2})";

        using PascalDocument = TDocument<false, false, true, detail::key_policy_pascal_case>;
        auto doc = PascalDocument::parse(json);

        REQUIRE(doc.ok());
        REQUIRE(doc.root().get("FooBar").as_i64() == 1);
        REQUIRE(doc.root().get("FooBaz").as_i64() == 2);
        REQUIRE(doc.root().contains("foo_bar"));
    }
}

TEST_CASE("Fold expression adds 100 elements", "[ujson][value_builder][fold]") {
    ValueBuilder b;
    auto root = b.root().set_object();

    auto add_items = [&](auto seq) {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (
                [&] {
                    auto added = root.add("k" + std::to_string(I), static_cast<std::int64_t>(I));
                    REQUIRE(added.ok());
                    REQUIRE(added.raw()->data.num.as_i64() == static_cast<std::int64_t>(I));
                }(),
                ...);
        }(seq);
    };

    add_items(std::make_index_sequence<100> {});

    REQUIRE(root.size() == 100);
    for (std::size_t i = 0; i < 100; ++i)
        REQUIRE(root.get("k" + std::to_string(i)).raw()->data.num.as_i64() == static_cast<std::int64_t>(i));
}

TEST_CASE("Insert or assign replaces existing value", "[ujson][value_builder][assign]") {
    ValueBuilder b;
    auto root = b.root().set_object();

    root.add("key", 1);
    root.add("key", 42);

    REQUIRE(root.get("key").raw()->data.num.i == 42);
}

TEST_CASE("Erase stable preserves order", "[ujson][value_builder][erase][stable]") {
    ValueBuilder b;
    auto root = b.root().set_object();

    root.add("a", 1);
    root.add("b", 2);
    root.add("c", 3);

    REQUIRE(root.erase("b", EraseMode::Stable));

    REQUIRE(root.size() == 2);
    REQUIRE(root.get("a").raw()->data.num.as_i64() == 1);
    REQUIRE(root.get("c").raw()->data.num.as_i64() == 3);
}

TEST_CASE("Erase fast swaps last", "[ujson][value_builder][erase][fast]") {
    ValueBuilder b;
    auto root = b.root().set_object();

    root.add("a", 1);
    root.add("b", 2);
    root.add("c", 3);

    REQUIRE(root.erase("a", EraseMode::Fast));
    REQUIRE(root.size() == 2);

    REQUIRE(root.contains("b"));
    REQUIRE(root.contains("c"));
}

TEST_CASE("Array auto expand", "[ujson][value_builder][array]") {
    ValueBuilder b;
    auto root = b.root().set_array();

    root[5] = 123;

    REQUIRE(root.size() == 6);
    REQUIRE(root[5].raw()->data.num.as_i64() == 123);
    REQUIRE(root[0].is_null());
}

TEST_CASE("Large scale insertion triggers rehash", "[ujson][value_builder][rehash][large]") {
    ValueBuilder::Options opt;
    opt.max_load = 0.70f;

    ValueBuilder b(opt);
    auto root = b.root().set_object();

    for (std::size_t i = 0; i < kLargeN; ++i) {
        root.add("key" + std::to_string(i), static_cast<std::int64_t>(i));
    }

    REQUIRE(root.size() == kLargeN);

    for (std::size_t i = 0; i < kLargeN; ++i) {
        auto v = root.get("key" + std::to_string(i));
        REQUIRE(v.raw()->data.num.as_i64() == static_cast<std::int64_t>(i));
    }

    REQUIRE(b.ok());
}

TEST_CASE("Mass erase triggers tombstone rebuild", "[ujson][value_builder][erase][tomb]") {
    ValueBuilder b;
    auto root = b.root().set_object();

    for (std::size_t i = 0; i < kMediumN; ++i)
        root.add("k" + std::to_string(i), i);

    for (std::size_t i = 0; i < kMediumN; i += 2)
        root.erase("k" + std::to_string(i));

    for (std::size_t i = 1; i < kMediumN; i += 2)
        REQUIRE(root.get("k" + std::to_string(i)).raw()->data.num.as_i64() == i);

    REQUIRE(b.ok());
}

TEST_CASE("Randomized stress insert/erase", "[ujson][value_builder][fuzz]") {
    ValueBuilder b;
    auto root = b.root().set_object();

    std::mt19937_64 rng(1337);
    std::unordered_set<std::string> keys;

    for (std::size_t i = 0; i < 20000; ++i) {
        auto id = std::to_string(rng() % 10000);
        if (rng() % 2) {
            root.add(id, i);
            keys.insert(id);
        } else {
            root.erase(id, (rng() % 2) ? EraseMode::Stable : EraseMode::Fast);
            keys.erase(id);
        }
    }

    for (const auto& k : keys) {
        REQUIRE(root.contains(k));
    }

    REQUIRE(b.ok());
}

TEST_CASE("String copy policy isolates lifetime", "[ujson][value_builder][string][copy]") {
    ValueBuilder::Options opt;
    opt.strings = StringPolicy::Copy;

    ValueBuilder b(opt);
    auto root = b.root().set_object();

    std::string tmp = "hello";
    tmp.reserve(32);
    root.add("x", tmp);

    tmp.replace(0, 5, "mutated");

    REQUIRE(root.get("x").raw()->data.str == "hello");
}

TEST_CASE("String view policy shares lifetime", "[ujson][value_builder][string][view]") {
    ValueBuilder::Options opt;
    opt.strings = StringPolicy::View;

    ValueBuilder b(opt);
    auto root = b.root().set_object();

    std::string tmp = "hello";
    root.add("x", tmp);

    tmp = "1ello";

    REQUIRE(root.get("x").raw()->data.str == "1ello");
}

TEST_CASE("Encoding policy utf8 stores utf8 bytes", "[ujson][value_builder][encoding][utf8]") {
    ValueBuilder::Options opt;
    opt.encoding = EncodingPolicy::Utf8;

    ValueBuilder b(opt);
    auto root = b.root().set_object();

    const std::u16string s = u"\u00A9\U0001F600";
    const std::u16string u16 = u"привет";
    const std::u32string u32 = U"привет";
    const std::wstring w = L"привет";
    const std::string utf8 = {static_cast<char>(0xD0), static_cast<char>(0xBF), static_cast<char>(0xD1), static_cast<char>(0x80), static_cast<char>(0xD0), static_cast<char>(0xB8),
                              static_cast<char>(0xD0), static_cast<char>(0xB2), static_cast<char>(0xD0), static_cast<char>(0xB5), static_cast<char>(0xD1), static_cast<char>(0x82)};
    root.add("s", s);
    root.add("utf8", utf8);
    root.add("u16", u16);
    root.add("u32", u32);
    root.add("w", w);

    const std::string expected = "\xC2\xA9\xF0\x9F\x98\x80";
    REQUIRE(root.get("s").raw()->data.str == expected);
    REQUIRE(root.get("utf8").raw()->data.str == utf8);
    REQUIRE(root.get("u16").raw()->data.str == utf8);
    REQUIRE(root.get("u32").raw()->data.str == utf8);
    REQUIRE(root.get("w").raw()->data.str == utf8);

    const std::string out = b.encode();
    REQUIRE(out == std::string("{\"s\":\"") + expected + "\",\"utf8\":\"" + utf8 + "\",\"u16\":\"" + utf8 + "\",\"u32\":\"" + utf8 + "\",\"w\":\"" + utf8 + "\"}");
    REQUIRE(out.find("\\u00") == std::string::npos);
}

TEST_CASE("Encoding policy utf8 escapes control characters", "[ujson][value_builder][encoding][utf8]") {
    ValueBuilder::Options opt;
    opt.encoding = EncodingPolicy::Utf8;

    ValueBuilder b(opt);
    auto root = b.root().set_object();

    root.add("s", std::string_view {"line\n\t\"quoted\"\\slash"});

    const std::string out = b.encode();
    REQUIRE(out == R"({"s":"line\n\t\"quoted\"\\slash"})");
}

TEST_CASE("Encoding policy json escaped stores ascii escapes", "[ujson][value_builder][encoding][json]") {
    ValueBuilder::Options opt;
    opt.encoding = EncodingPolicy::JsonEscaped;

    ValueBuilder b(opt);
    auto root = b.root().set_object();

    const std::u16string s = u"\u00A9\U0001F600";
    const std::u16string u16 = u"привет";
    const std::u32string u32 = U"привет";
    const std::wstring w = L"привет";
    const std::string utf8 = {static_cast<char>(0xD0), static_cast<char>(0xBF), static_cast<char>(0xD1), static_cast<char>(0x80), static_cast<char>(0xD0), static_cast<char>(0xB8),
                              static_cast<char>(0xD0), static_cast<char>(0xB2), static_cast<char>(0xD0), static_cast<char>(0xB5), static_cast<char>(0xD1), static_cast<char>(0x82)};
    root.add("s", s);
    root.add("utf8", utf8);
    root.add("u16", u16);
    root.add("u32", u32);
    root.add("w", w);

    const std::string expected = R"(\u00A9\uD83D\uDE00)";
    const std::string expected_utf8 = R"(\u043F\u0440\u0438\u0432\u0435\u0442)";
    REQUIRE(root.get("s").raw()->data.str == expected);
    REQUIRE(root.get("utf8").raw()->data.str == expected_utf8);
    REQUIRE(root.get("u16").raw()->data.str == expected_utf8);
    REQUIRE(root.get("u32").raw()->data.str == expected_utf8);
    REQUIRE(root.get("w").raw()->data.str == expected_utf8);

    const std::string out = b.encode();
    REQUIRE(out == std::string("{\"s\":\"") + expected + "\",\"utf8\":\"" + expected_utf8 + "\",\"u16\":\"" + expected_utf8 + "\",\"u32\":\"" + expected_utf8 + "\",\"w\":\"" + expected_utf8 + "\"}");

    for (const unsigned char c : out) {
        REQUIRE(c <= 0x7F);
    }
}

TEST_CASE("Encoding policy json escaped handles control characters", "[ujson][value_builder][encoding][json]") {
    ValueBuilder::Options opt;
    opt.encoding = EncodingPolicy::JsonEscaped;

    ValueBuilder b(opt);
    auto root = b.root().set_object();

    const std::u16string u16 = u"line\n\t\"quoted\"\\slash";
    const std::wstring w = L"line\n\t\"quoted\"\\slash";

    root.add("s", std::string_view {"line\n\t\"quoted\"\\slash"});
    root.add("u16", u16);
    root.add("w", w);

    const std::string expected = R"(line\n\t\"quoted\"\\slash)";
    REQUIRE(root.get("s").raw()->data.str == expected);
    REQUIRE(root.get("u16").raw()->data.str == expected);
    REQUIRE(root.get("w").raw()->data.str == expected);

    const std::string out = b.encode();
    REQUIRE(out == std::string("{\"s\":\"") + expected + "\",\"u16\":\"" + expected + "\",\"w\":\"" + expected + "\"}");

    for (const unsigned char c : out) {
        REQUIRE(c <= 0x7F);
    }
}

TEST_CASE("Nested structure heavy build", "[ujson][value_builder][nested]") {
    ValueBuilder b;
    auto root = b.root().set_object();

    auto statuses = root.add_array("statuses");

    for (std::size_t i = 0; i < 5000; ++i) {
        auto tweet = statuses.add_object();

        tweet.add("id", static_cast<std::int64_t>(1000000 + i));
        tweet.add("text", "payload");

        auto user = tweet.add_object("user");
        user.add("id", static_cast<std::int64_t>(5000 + i));
        user.add("name", "benchmark_user");
        user.add("verified", (i % 2) == 0);
    }

    REQUIRE(b.ok());
    REQUIRE(root.get("statuses").size() == 5000);
}

#if UJSON_VALIDATOR_STRICT_UTF8
TEST_CASE("validator: invalid utf8 bytes inside string rejected", "[ujson][validator][utf8]") {
    // UTF-8 sequence: 0xC3 0x28 invalid (bad continuation)
    std::string j = "{\"s\":\"";
    j.push_back(char(0xC3));
    j.push_back(char(0x28));
    j += "\"}";
    REQUIRE_FALSE(val_ok(j));
}

TEST_CASE("validator: overlong utf8 rejected", "[ujson][validator][utf8]") {
    // Overlong encoding of '/' => 0xC0 0xAF (invalid)
    std::string j = "{\"s\":\"";
    j.push_back(char(0xC0));
    j.push_back(char(0xAF));
    j += "\"}";
    REQUIRE_FALSE(val_ok(j));
}
#endif
