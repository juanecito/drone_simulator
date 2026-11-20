// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Minimal in-process HTTP/1.1 server for the network tests (SPECIFICATION §8.1: "net" tests run
// against an in-process mock server; CI never contacts real providers). One thread accepts
// connections on 127.0.0.1; each connection is answered on its own thread with
// `Connection: close`.

#pragma once

#include <atomic>
#include <cctype>
#include <chrono>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ds::test {

class MockHttpServer {
public:
    struct Request {
        std::string method;
        std::string target;                          // Path and query.
        std::map<std::string, std::string> headers;  // Lower-case names.
    };
    struct Response {
        int status = 200;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        std::chrono::milliseconds delay{0};
    };
    using Handler = std::function<Response(const Request&)>;

    explicit MockHttpServer(Handler handler) : handler_(std::move(handler)) {
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_, 64) != 0) {
            throw std::runtime_error("mock server: cannot listen");
        }
        socklen_t len = sizeof(addr);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        acceptor_ = std::thread([this] { acceptLoop(); });
    }

    ~MockHttpServer() {
        stop_.store(true);
        acceptor_.join();
        ::close(listen_);
        const std::scoped_lock lock(mutex_);
        for (std::thread& t : connections_) {
            t.join();
        }
    }

    MockHttpServer(const MockHttpServer&) = delete;
    MockHttpServer& operator=(const MockHttpServer&) = delete;

    [[nodiscard]] int port() const noexcept { return port_; }
    [[nodiscard]] std::string url(std::string_view path) const {
        return std::format("http://127.0.0.1:{}{}", port_, path);
    }
    [[nodiscard]] std::vector<Request> requests() const {
        const std::scoped_lock lock(mutex_);
        return requests_;
    }
    [[nodiscard]] std::size_t requestCount() const {
        const std::scoped_lock lock(mutex_);
        return requests_.size();
    }

private:
    void acceptLoop() {
        while (!stop_.load()) {
            pollfd p{listen_, POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0) {
                continue;
            }
            const int fd = ::accept(listen_, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            const std::scoped_lock lock(mutex_);
            connections_.emplace_back([this, fd] { serve(fd); });
        }
    }

    void serve(int fd) {
        std::string data;
        char buf[4096];
        while (data.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                ::close(fd);
                return;
            }
            data.append(buf, static_cast<std::size_t>(n));
        }
        Request req;
        std::string_view head(data.data(), data.find("\r\n\r\n"));
        std::size_t lineEnd = head.find("\r\n");
        const std::string_view first = head.substr(0, lineEnd);
        const std::size_t s1 = first.find(' ');
        const std::size_t s2 = first.find(' ', s1 + 1);
        req.method = std::string(first.substr(0, s1));
        req.target = std::string(first.substr(s1 + 1, s2 - s1 - 1));
        while (lineEnd != std::string_view::npos) {
            const std::size_t start = lineEnd + 2;
            lineEnd = head.find("\r\n", start);
            const std::string_view line = head.substr(start, lineEnd - start);
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            std::string name(line.substr(0, colon));
            for (char& c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
            req.headers[name] = std::string(value);
        }
        {
            const std::scoped_lock lock(mutex_);
            requests_.push_back(req);
        }
        const Response res = handler_(req);
        if (res.delay.count() > 0) {
            std::this_thread::sleep_for(res.delay);
        }
        std::string out =
            std::format("HTTP/1.1 {} Mock\r\nContent-Length: {}\r\nConnection: close\r\n",
                        res.status, res.body.size());
        for (const auto& [name, value] : res.headers) {
            out += std::format("{}: {}\r\n", name, value);
        }
        out += "\r\n";
        out += res.body;
        std::size_t sent = 0;
        while (sent < out.size()) {
            const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                break;
            }
            sent += static_cast<std::size_t>(n);
        }
        ::shutdown(fd, SHUT_WR);
        while (::recv(fd, buf, sizeof(buf), 0) > 0) {
        }
        ::close(fd);
    }

    Handler handler_;
    int listen_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread acceptor_;
    mutable std::mutex mutex_;
    std::vector<std::thread> connections_;
    std::vector<Request> requests_;
};

}  // namespace ds::test
