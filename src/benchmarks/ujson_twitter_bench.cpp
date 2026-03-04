#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ujson/ujson.hpp>

#include <rapidjson/document.h>
#include <rapidjson/reader.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <fstream>
#include <iostream>

#if defined(_WIN32)
    #define NOMINMAX
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace {

    struct MappedFile {
        const char* data = nullptr;
        size_t size = 0;

#if defined(_WIN32)
        HANDLE hFile = INVALID_HANDLE_VALUE;
        HANDLE hMap = nullptr;

        explicit MappedFile(const char* path) {
            hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            REQUIRE(hFile != INVALID_HANDLE_VALUE);

            LARGE_INTEGER li {};
            REQUIRE(GetFileSizeEx(hFile, &li));
            REQUIRE(li.QuadPart >= 0);

            size = static_cast<size_t>(li.QuadPart);
            REQUIRE(size > 0);

            hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
            REQUIRE(hMap != nullptr);

            void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
            REQUIRE(view != nullptr);

            data = static_cast<const char*>(view);
        }

        ~MappedFile() {
            if (data)
                UnmapViewOfFile(data);
            if (hMap)
                CloseHandle(hMap);
            if (hFile != INVALID_HANDLE_VALUE)
                CloseHandle(hFile);
        }
#else
        int fd = -1;

        explicit MappedFile(const char* path) {
            fd = ::open(path, O_RDONLY);
            REQUIRE(fd >= 0);

            struct stat st {};
            REQUIRE(::fstat(fd, &st) == 0);
            REQUIRE(st.st_size > 0);

            size = static_cast<size_t>(st.st_size);

            void* view = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            REQUIRE(view != MAP_FAILED);

            data = static_cast<const char*>(view);
        }

        ~MappedFile() {
            if (data)
                ::munmap(const_cast<char*>(data), size);
            if (fd >= 0)
                ::close(fd);
        }
#endif

        MappedFile(const MappedFile&) = delete;
        MappedFile& operator=(const MappedFile&) = delete;

        MappedFile(MappedFile&&) = delete;
        MappedFile& operator=(MappedFile&&) = delete;

        [[nodiscard]] std::string_view view() const noexcept {
            return {data, size};
        }
    };

    constexpr const char* kTwitterPath = UJSON_TEST_DATA_DIR "/twitter.json";

    struct ParsedDocument {
        ujson::Node* root_node = nullptr;
        ujson::Arena* arena = nullptr;
        ujson::DomContext ctx {};
        ujson::ParseError err {};

        [[nodiscard]] bool ok() const noexcept {
            return err.ok() && root_node;
        }

        [[nodiscard]] const ujson::ParseError& error() const noexcept {
            return err;
        }

        [[nodiscard]] ujson::ValueRef root() const noexcept {
            return ujson::ValueRef(root_node, &ctx);
        }
    };

    [[nodiscard]] ParsedDocument parse_document(const std::string_view json, ujson::Arena& arena, const std::uint32_t max_depth) {
        ParsedDocument doc {.root_node = nullptr, .arena = &arena, .ctx = {&arena, ujson::init_key_format(arena, ujson::detail::key_format::SnakeCase)}, .err = {}};

        ujson::SaxDomHandler builder {doc.ctx, max_depth};
        ujson::CoreParser<false, true, ujson::SaxDomHandler> parser(builder, json, max_depth);

        doc.err = parser.parse_root();
        if (!builder.ok())
            doc.err = builder.error();

        doc.root_node = builder.root();
        return doc;
    }

} // namespace

// -----------------------------
// DOM correctness (mmap, no copy)
// -----------------------------
TEST_CASE("ujson twitter DOM parse (mmap, no copy)", "[ujson][twitter]") {
    ujson::NewAllocator alloc {};
    ujson::Arena arena {alloc};

    MappedFile mf {kTwitterPath};
    const std::string_view json = mf.view();

    const auto doc = parse_document(json, arena, 512);
    std::cout << doc.error().format<ujson::ErrorFormat::Pretty>();
    REQUIRE(doc.ok());
    const auto root = doc.root();
    REQUIRE(root.is_object());

    const auto statuses = root.get("statuses");
    REQUIRE(statuses.is_array());
    REQUIRE(statuses.size() > 0);

    const auto first = statuses.at(0);
    REQUIRE(first.is_object());

    REQUIRE(first.contains("id"));
    REQUIRE(first.contains("text"));
    REQUIRE(first.contains("user"));

    const auto user = first.get("user");
    REQUIRE(user.is_object());
    REQUIRE(user.contains("id"));
    REQUIRE(user.contains("name"));
}

// -----------------------------
// Streaming SAX test (twitter)
// -----------------------------
struct TwitterSaxHandler {
    // simple streaming counters
    std::uint64_t objects = 0;
    std::uint64_t arrays = 0;
    std::uint64_t keys = 0;
    std::uint64_t strings = 0;
    std::uint64_t string_bytes = 0;
    std::uint64_t numbers = 0;
    std::uint64_t bools = 0;
    std::uint64_t nulls = 0;

    // twitter-specific: count statuses and text fields inside statuses
    std::uint64_t statuses_count = 0;
    std::uint64_t text_fields = 0;
    std::uint64_t text_bytes = 0;

    // state machine
    std::string_view last_key {};
    bool expecting_statuses_array = false;
    bool in_statuses_array = false;
    std::uint32_t statuses_array_depth = 0; // depth relative to start of statuses array

    bool on_null() {
        ++nulls;
        last_key = {};
        expecting_statuses_array = false;
        return true;
    }

    bool on_bool(bool) {
        ++bools;
        last_key = {};
        expecting_statuses_array = false;
        return true;
    }

    bool on_integer(std::int64_t) {
        ++numbers;
        last_key = {};
        expecting_statuses_array = false;
        return true;
    }

    bool on_number(double) {
        ++numbers;
        last_key = {};
        expecting_statuses_array = false;
        return true;
    }

    bool on_string(std::string_view s) {
        ++strings;
        string_bytes += s.size();

        if (in_statuses_array && last_key == "text") {
            ++text_fields;
            text_bytes += s.size();
        }

        last_key = {};
        expecting_statuses_array = false;
        return true;
    }

    bool on_key(std::string_view k) {
        ++keys;
        last_key = k;
        expecting_statuses_array = (k == "statuses");
        return true;
    }

    bool on_array_begin() {
        ++arrays;

        if (expecting_statuses_array && !in_statuses_array) {
            in_statuses_array = true;
            statuses_array_depth = 0;
        } else if (in_statuses_array) {
            ++statuses_array_depth;
        }

        last_key = {};
        expecting_statuses_array = false;
        return true;
    }

    bool on_array_end() {
        if (in_statuses_array) {
            if (statuses_array_depth == 0) {
                in_statuses_array = false;
            } else {
                --statuses_array_depth;
            }
        }

        last_key = {};
        expecting_statuses_array = false;
        return true;
    }

    bool on_object_begin() {
        ++objects;
        if (in_statuses_array && statuses_array_depth == 0) {
            // Each element of statuses array is typically an object (a tweet)
            ++statuses_count;
        }
        last_key = {};
        expecting_statuses_array = false;
        return true;
    }

    bool on_object_end() {
        last_key = {};
        expecting_statuses_array = false;
        return true;
    }
};

TEST_CASE("twitter compare ujson vs rapidjson (617KB)", "[bench][twitter][vs]") {
    MappedFile mf {kTwitterPath};
    const std::string_view json = mf.view();
    REQUIRE(!json.empty());

    // ---- Warmup (avoid cold cache distortion)
    {
        ujson::NewAllocator alloc {};
        ujson::Arena arena {alloc};
        auto doc = parse_document(json, arena, 512);
        REQUIRE(doc.ok());

        rapidjson::Document rdoc;
        rdoc.Parse(json.data(), json.size());
        REQUIRE(!rdoc.HasParseError());
    }

    SECTION("DOM Parse") {

        BENCHMARK("ujson DOM twitter") {
            ujson::NewAllocator alloc {};
            ujson::Arena arena {alloc};
            auto doc = parse_document(json, arena, 512);
            REQUIRE(doc.ok());
            return doc.root().size(); // prevent optimization
        };

        BENCHMARK("rapidjson DOM twitter") {
            rapidjson::Document rdoc;
            rdoc.Parse(json.data(), json.size());
            REQUIRE(!rdoc.HasParseError());
            return rdoc.MemberCount();
        };
    }

    SECTION("SAX Parse") {

        BENCHMARK("ujson SAX twitter") {
            TwitterSaxHandler handler {};
            ujson::SaxParser parser(handler, json, 512);
            auto err = parser.parse();
            REQUIRE(err.ok());
            return handler.strings;
        };

        BENCHMARK("rapidjson SAX twitter") {
            struct DummyHandler : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, DummyHandler> {
                std::uint64_t strings = 0;
                bool String(const char*, rapidjson::SizeType, bool) {
                    ++strings;
                    return true;
                }
                bool Null() {
                    return true;
                }
                bool Bool(bool) {
                    return true;
                }
                bool Int(int) {
                    return true;
                }
                bool Uint(unsigned) {
                    return true;
                }
                bool Int64(std::int64_t) {
                    return true;
                }
                bool Uint64(std::uint64_t) {
                    return true;
                }
                bool Double(double) {
                    return true;
                }
                bool RawNumber(const char*, rapidjson::SizeType, bool) {
                    return true;
                }
                bool StartObject() {
                    return true;
                }
                bool Key(const char*, rapidjson::SizeType, bool) {
                    return true;
                }
                bool EndObject(rapidjson::SizeType) {
                    return true;
                }
                bool StartArray() {
                    return true;
                }
                bool EndArray(rapidjson::SizeType) {
                    return true;
                }
            };

            DummyHandler handler;

            // insitu parse for fairness
            std::vector<char> buffer(json.begin(), json.end());
            buffer.push_back('\0');

            rapidjson::Reader reader;
            rapidjson::InsituStringStream stream(buffer.data());

            REQUIRE(reader.Parse(stream, handler));
            return handler.strings;
        };
    }

    SECTION("Encode") {

        // Pre-parse once for both libraries
        ujson::NewAllocator alloc {};
        ujson::Arena arena {alloc};
        auto doc = parse_document(json, arena, 512);
        REQUIRE(doc.ok());

        rapidjson::Document rdoc;
        rdoc.Parse(json.data(), json.size());
        REQUIRE(!rdoc.HasParseError());

        BENCHMARK("ujson encode twitter") {
            ujson::ParseError err {};
            auto out = ujson::encode(doc.root(), false, &err);
            REQUIRE(err.ok());
            return out.size();
        };

        BENCHMARK("rapidjson encode twitter") {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            rdoc.Accept(writer);
            return buffer.GetSize();
        };
    }
}

TEST_CASE("ujson twitter SAX streaming parse (mmap, no copy)", "[ujson][twitter][sax]") {
    MappedFile mf {kTwitterPath};
    const std::string_view json = mf.view();

    TwitterSaxHandler h {};

    ujson::SaxParser p(h, json, 512);
    REQUIRE(p.parse().ok());

    REQUIRE(h.keys > 0);
    REQUIRE(h.objects > 0);
    REQUIRE(h.arrays > 0);

    // twitter sanity: should see statuses + some texts (for common twitter samples)
    REQUIRE(h.statuses_count > 0);
    REQUIRE(h.text_fields > 0);
    REQUIRE(h.text_bytes > 0);
}

// -----------------------------
// Deep twitter field traversal benchmark
// (DOM: iterate statuses, lookup nested fields)
// -----------------------------
TEST_CASE("ujson twitter deep field traversal benchmark (DOM, mmap)", "[ujson][twitter][bench]") {
    ujson::NewAllocator alloc {};
    ujson::Arena arena {alloc};

    MappedFile mf {kTwitterPath};
    const std::string_view json = mf.view();

    const auto doc = parse_document(json, arena, 512);
    REQUIRE(doc.ok());

    const auto root = doc.root();
    const auto statuses = root.get("statuses");
    REQUIRE(statuses.is_array());

    // Warm up indexes once (objindex builds lazily on get())
    if (statuses.size() > 0) {
        const auto first = statuses.at(0);
        (void)first.get("user");
        (void)first.get("text");
        const auto user = first.get("user");
        (void)user.get("id");
        (void)user.get("name");
        (void)user.get("screen_name");
    }

    BENCHMARK("iterate statuses + lookup user.id/name/text") {
        std::uint64_t acc = 0;
        for (auto v : statuses.items()) {
            // v is ValueRef
            const auto text = v.get("text");
            const auto user = v.get("user");

            acc += text.as_string().size();
            acc += static_cast<std::uint64_t>(user.get("id").as_i64());
            acc += user.get("name").as_string().size();
            acc += user.get("screen_name").as_string().size();
        }

        return acc;
    };
}

// -----------------------------
// Twitter string-heavy benchmark
// (SAX: count strings/bytes + text bytes)
// -----------------------------
TEST_CASE("ujson twitter string-heavy benchmark (SAX, mmap)", "[ujson][twitter][bench][sax]") {
    MappedFile mf {kTwitterPath};
    const std::string_view json = mf.view();

    BENCHMARK("SAX count strings + bytes + text bytes") {
        TwitterSaxHandler h {};
        ujson::SaxParser p(h, json, 512);
        if (const bool ok = p.parse().ok(); !ok)
            return std::uint64_t {0};

        // return something so compiler can't DCE everything
        return h.strings + h.string_bytes + h.text_bytes + h.statuses_count;
    };
}

static void save_file(const char* path, const std::string_view data) {
    std::ofstream f(path, std::ios::binary);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

static std::uint64_t hash_sv32(const std::string_view s) {
    return ujson::hash32(s.data(), s.size());
}

class ValueBuilderSaxHandler {
public:
    explicit ValueBuilderSaxHandler(ujson::ValueBuilder& b, std::uint32_t max_depth = 512): builder_(b) {
        stack_.reserve(max_depth);
    }

    bool on_null() {
        write(nullptr);
        return true;
    }
    bool on_bool(bool v) {
        write(v);
        return true;
    }
    bool on_integer(std::int64_t v) {
        write(v);
        return true;
    }
    bool on_number(double v) {
        write(v);
        return true;
    }
    bool on_string(std::string_view s) {
        write(s);
        return true;
    }

    bool on_key(std::string_view k) {
        current_key_ = k;
        return true;
    }

    bool on_object_begin() {
        if (stack_.empty()) {
            // root object
            auto root = builder_.root().set_object();
            stack_.push_back(root);
        } else {
            auto child = create_child_object();
            stack_.push_back(child);
        }
        return true;
    }

    bool on_object_end() {
        stack_.pop_back();
        return true;
    }

    bool on_array_begin() {
        if (stack_.empty()) {
            // root array
            auto root = builder_.root().set_array();
            stack_.push_back(root);
        } else {
            auto child = create_child_array();
            stack_.push_back(child);
        }
        return true;
    }

    bool on_array_end() {
        stack_.pop_back();
        return true;
    }

private:
    ujson::ValueBuilder& builder_;
    std::vector<ujson::NodeRef> stack_;
    std::string_view current_key_;

    ujson::NodeRef& current() {
        return stack_.back();
    }

    ujson::NodeRef create_child_object() {
        auto& parent = current();
        if (parent.is_array())
            return parent.add_object();
        return parent.add_object(current_key_);
    }

    ujson::NodeRef create_child_array() {
        auto& parent = current();
        if (parent.is_array())
            return parent.add_array();
        return parent.add_array(current_key_);
    }

    template <class T>
    void write(T&& v) {
        auto& node = current();

        if (node.is_array()) {
            node.add(std::forward<T>(v));
        } else {
            node.add(current_key_, std::forward<T>(v));
        }
    }
};

TEST_CASE("ujson twitter: SAX -> DOM -> encode (streaming transform)", "[ujson][twitter][sax][build][encode]") {
    MappedFile mf {kTwitterPath};
    const std::string_view input = mf.view();

    REQUIRE(!input.empty());

    constexpr std::uint32_t kMaxDepth = 512;

    BENCHMARK("twitter: SAX->ValueBuilder->encode->save") {
        ujson::NewAllocator<> alloc;
        ujson::Arena arena {alloc};

        ujson::ValueBuilder builder {{.strings = ujson::StringPolicy::View}};

        ValueBuilderSaxHandler handler {builder};

        ujson::SaxParser parser {handler, std::string_view {input}, arena, kMaxDepth};

        REQUIRE(parser.parse().ok());
        REQUIRE(builder.ok());

        const std::string out = builder.encode();
        REQUIRE(!out.empty());

        save_file("twitter_vb_out.json", out);

        return out.size();
    };

    // --------- 1) SAX -> DOM -> encode -> save ---------
    BENCHMARK("twitter: SAX->DomBuilder->encode->save") {
        ujson::NewAllocator alloc;
        ujson::Arena arena {alloc};

        ujson::DomContext ctx {&arena, ujson::init_key_format(arena, ujson::detail::key_format::SnakeCase)};
        ujson::SaxDomHandler builder {ctx, kMaxDepth};

        ujson::SaxParser parser {builder, std::string_view {input}, arena, kMaxDepth};

        REQUIRE(parser.parse().ok());
        REQUIRE(builder.ok());
        REQUIRE(builder.root());

        // encode
        ujson::ParseError werr {};
        const std::string out = ujson::encode(ujson::ValueRef(builder.root(), &ctx), false, &werr);
        REQUIRE(werr.ok());
        REQUIRE(!out.empty());
        save_file("twitter_out.json", out);

        return out.size();
    };

    // --------- 2) SAX -> DOM only ---------
    BENCHMARK("twitter: SAX->DomBuilder (build only)") {
        ujson::NewAllocator alloc;
        ujson::Arena arena {alloc};

        ujson::DomContext ctx {&arena, ujson::init_key_format(arena, ujson::detail::key_format::SnakeCase)};
        ujson::SaxDomHandler builder {ctx, kMaxDepth};

        ujson::SaxParser parser {builder, std::string_view {input}, arena, kMaxDepth};
        if (const bool ok = parser.parse().ok(); !ok || !builder.ok() || !builder.root())
            return std::size_t {0};

        return static_cast<std::size_t>(ujson::ValueRef(builder.root(), &ctx).size());
    };

    // --------- 3) encode only (DOM built once outside benchmark) ---------
    ujson::NewAllocator alloc;
    ujson::Arena arena {alloc};

    auto doc = parse_document(std::string_view {input}, arena, kMaxDepth);
    REQUIRE(doc.ok());

    BENCHMARK("twitter: encode only (from DOM)") {
        ujson::ParseError werr {};
        const std::string out = ujson::encode(doc.root(), false, &werr);
        if (!werr.ok())
            return std::size_t {0};
        return out.size();
    };

    // --------- 4) deep traversal benchmark ---------
    BENCHMARK("twitter: deep traversal (statuses[].user.screen_name, id, text)") {
        const auto root = doc.root();

        std::uint64_t acc = 0;

        const auto statuses = root.get("statuses");
        if (!statuses.is_array())
            return std::uint64_t {0};
        const auto status_items = statuses.items();
        for (auto v : status_items) {
            // id
            acc += static_cast<std::uint64_t>(v.get("id").as_i64(0));

            // text (string heavy)
            acc ^= hash_sv32(v.get("text").as_string());

            // user.screen_name
            const auto user = v.get("user");
            acc ^= hash_sv32(user.get("screen_name").as_string());

            // entities.hashtags[].text
            const auto entities = v.get("entities");
            if (const auto hashtags = entities.get("hashtags"); hashtags.is_array()) {
                const auto hashtag_items = hashtags.items();
                for (auto h : hashtag_items) {
                    acc ^= (hash_sv32(h.get("text").as_string()) * 0x9E3779B97F4A7C15ull);
                }
            }
        }

        return acc;
    };

    // --------- 5) string-heavy benchmark ---------
    BENCHMARK("twitter: string-heavy (hash lots of strings)") {
        const auto root = doc.root();
        const auto statuses = root.get("statuses");
        if (!statuses.is_array())
            return std::uint64_t {0};

        std::uint64_t acc = 0;
        const auto status_items = statuses.items();
        for (auto v : status_items) {
            acc ^= hash_sv32(v.get("text").as_string());
            acc ^= hash_sv32(v.get("source").as_string());

            const auto user = v.get("user");
            acc ^= hash_sv32(user.get("name").as_string());
            acc ^= hash_sv32(user.get("screen_name").as_string());
            acc ^= hash_sv32(user.get("location").as_string());
            acc ^= hash_sv32(user.get("description").as_string());

            const auto entities = v.get("entities");
            const auto urls = entities.get("urls");
            if (urls.is_array()) {
                const auto url_items = urls.items();
                for (auto u : url_items) {
                    acc ^= hash_sv32(u.get("expanded_url").as_string());
                    acc ^= hash_sv32(u.get("display_url").as_string());
                }
            }
        }

        return acc;
    };
}