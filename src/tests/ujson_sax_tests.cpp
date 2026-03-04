#include <catch2/catch_test_macros.hpp>

#include <ujson/ujson.hpp>

#include <sstream>
#include <string>
#include <vector>

using namespace ujson;

using TestDocument = DocumentView;

namespace {

    struct RecordingHandler {
        std::vector<std::string> events;

        bool on_null() {
            events.emplace_back("null");
            return true;
        }

        bool on_bool(const bool b) {
            events.emplace_back(b ? "true" : "false");
            return true;
        }

        bool on_number(const double d) {
            std::ostringstream ss;
            ss << "num:" << d;
            events.emplace_back(ss.str());
            return true;
        }

        bool on_integer(const std::int64_t v) {
            std::ostringstream ss;
            ss << "num:" << v;
            events.emplace_back(ss.str());
            return true;
        }

        bool on_string(const std::string_view s) {
            events.emplace_back(std::string("str:") + std::string(s));
            return true;
        }

        bool on_object_begin() {
            events.emplace_back("{");
            return true;
        }

        bool on_object_end() {
            events.emplace_back("}");
            return true;
        }

        bool on_array_begin() {
            events.emplace_back("[");
            return true;
        }

        bool on_array_end() {
            events.emplace_back("]");
            return true;
        }

        bool on_key(const std::string_view k) {
            events.emplace_back(std::string("key:") + std::string(k));
            return true;
        }
    };

} // namespace

TEST_CASE("ujson sax primitives", "[ujson][sax]") {
    std::string json = R"(null true false 123 "abc")";

    RecordingHandler h;
    SaxParser parser(h, json);

    REQUIRE(parser.parse().ok() == false); // multiple root values → invalid
}

TEST_CASE("ujson sax object", "[ujson][sax]") {
    std::string json = R"({"a":1,"b":true,"c":"x"})";

    RecordingHandler h;
    SaxParser parser(h, json);

    REQUIRE(parser.parse());

    std::vector<std::string> expected = {"{", "key:a", "num:1", "key:b", "true", "key:c", "str:x", "}"};

    REQUIRE(h.events == expected);
}

TEST_CASE("ujson sax key policy", "[ujson][sax][key_policy]") {
    std::string json = R"({"fooBar":1})";

    RecordingHandler h;
    SaxParser<RecordingHandler, detail::key_policy_snake_case> parser(h, json);

    REQUIRE(parser.parse());

    std::vector<std::string> expected = {"{", "key:foo_bar", "num:1", "}"};
    REQUIRE(h.events == expected);
}

TEST_CASE("ujson sax nested", "[ujson][sax]") {
    std::string json = R"({"a":[1,2,{"b":3}]})";

    RecordingHandler h;
    SaxParser parser(h, json);

    REQUIRE(parser.parse());

    std::vector<std::string> expected = {"{", "key:a", "[", "num:1", "num:2", "{", "key:b", "num:3", "}", "]", "}"};

    REQUIRE(h.events == expected);
}

TEST_CASE("ujson sax depth limit", "[ujson][sax]") {
    std::string json = "[[[[[[[[[[]]]]]]]]]]";

    RecordingHandler h;
    SaxParser parser(h, json, 5);

    REQUIRE(parser.parse().ok() == false);
}

TEST_CASE("ujson sax early stop", "[ujson][sax]") {
    struct StopAfterFirst {
        int count = 0;

        bool on_null() {
            return true;
        }
        bool on_bool(bool) {
            return true;
        }

        bool on_number(double) {
            ++count;
            return false; // stop immediately
        }

        bool on_integer(std::int64_t) {
            ++count;
            return false;
        }

        bool on_string(std::string_view) {
            return true;
        }
        bool on_object_begin() {
            return true;
        }
        bool on_object_end() {
            return true;
        }
        bool on_array_begin() {
            return true;
        }
        bool on_array_end() {
            return true;
        }
        bool on_key(std::string_view) {
            return true;
        }
    };

    StopAfterFirst h;
    std::string json = "[1,2,3,4]";

    SaxParser parser(h, json);
    REQUIRE(parser.parse().ok() == false);
    REQUIRE(h.count == 1);
}

TEST_CASE("ujson sax vs dom count", "[ujson][sax]") {
    std::string json = R"({"a":[1,2,3],"b":{"c":4}})";

    NewAllocator alloc{};
    Arena arena{alloc};

    auto doc = TestDocument::parse(json, arena);
    REQUIRE(doc.ok());

    int dom_count = 0;

    doc.root().for_each([&](ValueRef v) { ++dom_count; });

    int sax_values = 0;

    struct Counter {
        int& c;
        bool on_null() {
            ++c;
            return true;
        }
        bool on_bool(bool) {
            ++c;
            return true;
        }
        bool on_number(double) {
            ++c;
            return true;
        }

        bool on_integer(std::int64_t) {
            ++c;
            return true;
        }
        bool on_string(std::string_view) {
            ++c;
            return true;
        }
        bool on_object_begin() {
            return true;
        }
        bool on_object_end() {
            return true;
        }
        bool on_array_begin() {
            return true;
        }
        bool on_array_end() {
            return true;
        }
        bool on_key(std::string_view) {
            return true;
        }
    };

    Counter h{sax_values};
    SaxParser parser(h, json);

    REQUIRE(parser.parse());

    REQUIRE(sax_values >= dom_count);
}

TEST_CASE("ujson sax large array", "[ujson][sax]") {
    std::string json = "[";
    for (int i = 0; i < 10000; ++i) {
        json += std::to_string(i);
        if (i != 9999)
            json += ",";
    }
    json += "]";

    int count = 0;

    struct Counter {
        int& c;
        bool on_null() {
            return true;
        }
        bool on_bool(bool) {
            return true;
        }
        bool on_number(double) {
            ++c;
            return true;
        }

        bool on_integer(std::int64_t) {
            ++c;
            return true;
        }
        bool on_string(std::string_view) {
            return true;
        }
        bool on_object_begin() {
            return true;
        }
        bool on_object_end() {
            return true;
        }
        bool on_array_begin() {
            return true;
        }
        bool on_array_end() {
            return true;
        }
        bool on_key(std::string_view) {
            return true;
        }
    };

    Counter h{count};
    SaxParser parser(h, json);

    REQUIRE(parser.parse());
    REQUIRE(count == 10000);
}

TEST_CASE("sax counts tokens", "[ujson][sax]") {
    struct Counter {
        int objects = 0;
        int arrays = 0;
        int numbers = 0;

        bool on_null() {
            return true;
        }
        bool on_bool(bool) {
            return true;
        }
        bool on_number(double) {
            ++numbers;
            return true;
        }

        bool on_integer(std::int64_t) {
            ++numbers;
            return true;
        }
        bool on_string(std::string_view) {
            return true;
        }
        bool on_object_begin() {
            ++objects;
            return true;
        }
        bool on_object_end() {
            return true;
        }
        bool on_array_begin() {
            ++arrays;
            return true;
        }
        bool on_array_end() {
            return true;
        }
        bool on_key(std::string_view) {
            return true;
        }
    };

    std::string json = R"({"a":[1,2,3],"b":{"x":5}})";
    Counter c;
    SaxParser parser(c, json);

    REQUIRE(parser.parse());
    CHECK(c.objects == 2);
    CHECK(c.arrays == 1);
    CHECK(c.numbers == 4);
}
