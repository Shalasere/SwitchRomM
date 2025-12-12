#include <iostream>
#include <string>
#include "api_test_hooks.hpp"

struct TestRunner {
    int passed{0};
    int failed{0};

    void expect(bool cond, const std::string& name) {
        if (cond) {
            passed++;
        } else {
            failed++;
            std::cerr << "FAIL: " << name << "\n";
        }
    }
};

int main() {
    TestRunner t;

    {
        std::string host, port, path, err;
        bool ok = romm::parseHttpUrl("http://example.com:8080/path?x=1", host, port, path, err);
        t.expect(ok, "parseHttpUrl http ok");
        t.expect(host == "example.com", "parseHttpUrl host");
        t.expect(port == "8080", "parseHttpUrl port");
        t.expect(path == "/path?x=1", "parseHttpUrl path");
    }

    {
        std::string host, port, path, err;
        bool ok = romm::parseHttpUrl("https://bad.com", host, port, path, err);
        t.expect(!ok, "parseHttpUrl rejects https");
        t.expect(!err.empty(), "parseHttpUrl https err set");
    }

    {
        std::string decoded;
        std::string body = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
        bool ok = romm::decodeChunkedBody(body, decoded);
        t.expect(ok, "decodeChunkedBody valid");
        t.expect(decoded == "Wikipedia", "decodeChunkedBody content");
    }

    {
        std::string decoded;
        std::string body = "4\r\nWiki\r\nZ\r\nbad\r\n0\r\n\r\n"; // malformed chunk size
        bool ok = romm::decodeChunkedBody(body, decoded);
        t.expect(!ok, "decodeChunkedBody malformed");
    }

    if (t.failed == 0) {
        std::cout << "All tests passed (" << t.passed << ")\n";
        return 0;
    }
    std::cout << "Tests passed: " << t.passed << " failed: " << t.failed << "\n";
    return 1;
}
