#pragma once

#include "updater/worker.hpp"

#include <boost/asio/io_context.hpp>

namespace updater {

struct HttpOptions {
    std::string address = "127.0.0.1";
    std::uint16_t port = 8080;
    std::ptrdiff_t maximum_connections = 128;
    std::ptrdiff_t header_bytes = 8192;
    std::ptrdiff_t target_bytes = 256;
    std::chrono::milliseconds read_timeout = std::chrono::seconds(5);
    std::chrono::milliseconds write_timeout = std::chrono::seconds(10);
};
struct HealthReply {
    bool ready;
    std::string body;
};
using HealthProvider = std::function<HealthReply()>;

// Start, stop, and status are called on the I/O thread (or before run()).
class HttpServer {
public:
    HttpServer(boost::asio::io_context& context, RequestWorker& worker,
               HttpOptions options = {}, HealthProvider health = {});
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    [[nodiscard]] Result<std::uint16_t> start();
    void stop(bool drain_responses = true);
    [[nodiscard]] std::ptrdiff_t sessions() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace updater
