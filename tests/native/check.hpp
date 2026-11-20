#pragma once

// Minimal registry for the native ports of the Python reference tests. Each
// TEST_CASE runs independently; a failure reports its name and the first
// failed check, and the remaining cases still run.

#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace native_test {

namespace fs = std::filesystem;

struct Context {
    fs::path root;  // repository root
    fs::path work;  // writable directory for this test executable
    fs::path cli;   // build/braille, when the suite passes it
};

inline Context& context() {
    static Context value;
    return value;
}

using Case = std::pair<std::string, std::function<void()>>;

inline std::vector<Case>& cases() {
    static std::vector<Case> value;
    return value;
}

struct Register {
    Register(std::string name, std::function<void()> body) { cases().emplace_back(std::move(name), std::move(body)); }
};

std::string printable(const std::string& value);
std::string printable(const std::u32string& value);
template <typename T> std::string printable(const std::vector<T>& values);
template <typename T> std::string printable(const T& value);

inline std::string printable(const std::string& value) { return '"' + value + '"'; }

inline std::string printable(const std::u32string& value) {
    std::string out;
    for (char32_t c : value) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return '"' + out + '"';
}

template <typename T>
std::string printable(const std::vector<T>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) out += (i ? ", " : "") + printable(values[i]);
    return out + "]";
}

template <typename T>
std::string printable(const T& value) {
    if constexpr (requires { std::to_string(value); }) return std::to_string(value);
    else return "<value>";
}

inline void check(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}

template <typename A, typename B>
void check_equal(const A& actual, const B& expected, const std::string& detail) {
    if (!(actual == expected))
        throw std::runtime_error(detail + "\n  actual:   " + printable(actual) + "\n  expected: " + printable(expected));
}

// The callable must throw an exception whose message contains every fragment.
inline void expect_error(const std::function<void()>& body, std::initializer_list<std::string_view> fragments,
                         const std::string& detail = {}) {
    std::string prefix = detail.empty() ? std::string() : detail + ": ";
    try {
        body();
    } catch (const std::exception& error) {
        std::string message = error.what();
        for (auto fragment : fragments)
            check(message.find(fragment) != std::string::npos,
                  prefix + "error lacks \"" + std::string(fragment) + "\": " + message);
        return;
    }
    throw std::runtime_error(prefix + "expected an error");
}

// Usage: test_suite ROOT WORK [CLI] [--only NAME-SUBSTRING]
inline int run(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " ROOT WORK [CLI] [--only NAME]\n";
        return 2;
    }
    auto& ctx = context();
    ctx.root = argv[1];
    ctx.work = argv[2];
    std::string only;
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--only" && i + 1 < argc) only = argv[++i];
        else ctx.cli = arg;
    }
    fs::create_directories(ctx.work);
    int failed = 0, ran = 0;
    for (auto& [name, body] : cases()) {
        if (!only.empty() && name.find(only) == std::string::npos) continue;
        ++ran;
        try {
            body();
            std::cout << "ok   " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << ran - failed << " passed, " << failed << " failed\n";
    return failed ? 1 : (ran || only.empty() ? 0 : 3);
}

} // namespace native_test

#define NATIVE_TEST_JOIN2(a, b) a##b
#define NATIVE_TEST_JOIN(a, b) NATIVE_TEST_JOIN2(a, b)
#define TEST_CASE(name)                                                              \
    static void NATIVE_TEST_JOIN(native_case_, __LINE__)();                          \
    static native_test::Register NATIVE_TEST_JOIN(native_register_, __LINE__)(name, \
        NATIVE_TEST_JOIN(native_case_, __LINE__));                                   \
    static void NATIVE_TEST_JOIN(native_case_, __LINE__)()
