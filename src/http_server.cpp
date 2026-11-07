// Shared Updater Server - src/http_server.cpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#include "updater/http_server.hpp"
#include "updater/json.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <atomic>
#include <format>
#include <map>
#include <utility>
#endif

namespace updater {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;
using Error = boost::system::error_code;

struct HttpServer::Impl : std::enable_shared_from_this<HttpServer::Impl> {
    struct Session;
    struct Connections {
        std::atomic<std::ptrdiff_t> count{};
        std::weak_ptr<Impl> owner;
    };
    struct Connection {
        explicit Connection(std::shared_ptr<Connections> state) : state(std::move(state)) { ++this->state->count; }

        ~Connection();
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        std::shared_ptr<Connections> state;
    };
    net::io_context& context;
    RequestWorker& worker;
    HttpOptions options;
    HealthProvider health;
    tcp::acceptor acceptor;
    net::steady_timer retry;
    std::shared_ptr<Connections> connections = std::make_shared<Connections>();
    std::map<std::int64_t, std::shared_ptr<Session>> live;
    std::int64_t next_id = 1;
    bool accepting = false;
    bool stopping = false;
    bool retry_pending = false;
    MonoTime retry_after{};

    Impl(net::io_context& context, RequestWorker& worker, HttpOptions options, HealthProvider health)
        : context(context), worker(worker), options(std::move(options)), health(std::move(health)),
          acceptor(context), retry(context) {}

    void accept();
    void remove(std::int64_t id) { live.erase(id); }
    void stop(bool drain);
};

struct HttpServer::Impl::Session : std::enable_shared_from_this<HttpServer::Impl::Session> {
    enum class State { reading, awaiting, writing, closed };
    Connection connection;
    std::weak_ptr<Impl> owner;
    RequestWorker& worker;
    HttpOptions options;
    std::int64_t id;
    beast::tcp_stream stream;
    beast::flat_buffer buffer;
    http::request_parser<http::empty_body> parser;
    net::steady_timer queue_timer;
    std::optional<Submission> submission;
    std::optional<http::response<http::string_body>> response;
    State state = State::reading;
    bool write_pending = false;
    bool head = false;

    Session(const std::shared_ptr<Impl>& parent, std::int64_t id)
        : connection(parent->connections), owner(parent), worker(parent->worker), options(parent->options),
          id(id), stream(parent->context), buffer(static_cast<std::size_t>(options.header_bytes)),
          queue_timer(parent->context)
    {
        parser.header_limit(static_cast<std::uint32_t>(options.header_bytes));
        parser.body_limit(0);
    }

    void start()
    {
        stream.expires_after(options.read_timeout);
        http::async_read_header(stream, buffer, parser,
            [self = shared_from_this()](Error error, std::size_t) { self->read(error); });
    }

    void read(Error error)
    {
        if (state == State::closed) return;
        head = parser.get().method() == http::verb::head;
        if (error) {
            if (error == net::error::operation_aborted || error == beast::error::timeout
                || error == http::error::end_of_stream || error == net::error::eof) { close(); return; }
            send(error == http::error::header_limit ? 431 : 400, "invalid_request\n");
            return;
        }
        const auto& request = parser.get();
        if ((request.version() >= 11 && (request.count(http::field::host) != 1
             || request[http::field::host].empty()))
            || request.find(http::field::transfer_encoding) != request.end()
            || parser.content_length().value_or(0) != 0
            || request.find(http::field::expect) != request.end()) {
            send(400, "invalid_request\n");
            return;
        }
        if (request.target().size() > static_cast<std::size_t>(options.target_bytes)) {
            send(400, "invalid_target\n");
            return;
        }
        if (request.method() != http::verb::get) { send(405, "method_not_allowed\n"); return; }
        const std::string_view target(request.target().data(), request.target().size());
        if (target == "/healthz") {
            auto parent = owner.lock();
            if (!parent) { close(); return; }
            auto health = parent->health();
            if (health.body.size() > 1024) { send(503, "health_unavailable\n"); return; }
            send(health.ready ? 200 : 503, std::move(health.body), "application/json; charset=utf-8");
            return;
        }
        auto normalized = parse_check_target(target);
        if (!normalized) {
            send(normalized.error() == RouteError::not_found ? 404 : 400, "invalid_route\n");
            return;
        }
        const std::weak_ptr<Session> weak = shared_from_this();
        const auto executor = stream.get_executor();
        auto accepted = worker.try_submit(std::move(*normalized),
            [weak, executor](Completion completion) {
                net::post(executor, [weak, completion = std::move(completion)]() mutable {
                    if (auto self = weak.lock(); self && self->state == State::awaiting)
                        self->complete(std::move(completion));
                });
            });
        if (!accepted) { send(503, "busy\n"); return; }
        submission.emplace(std::move(*accepted));
        state = State::awaiting;
        stream.expires_never();
        queue_timer.expires_at(submission->queue_deadline);
        queue_timer.async_wait([weak](Error error) {
            if (!error)
                if (auto self = weak.lock(); self && self->state == State::awaiting && self->submission)
                    self->worker.cancel(self->submission->handle, CancelReason::queue_timeout);
        });
    }

    void complete(Completion completion)
    {
        if (!submission || completion.sequence != submission->handle.sequence) return;
        queue_timer.cancel();
        if (completion.reply.outcome == Outcome::ok)
            send(200, std::move(completion.reply.body), "application/atom+xml; charset=utf-8");
        else if (completion.reply.outcome == Outcome::not_found) send(404, "unknown_suite\n");
        else send(503, "unavailable\n");
    }

    void send(unsigned status, std::string body, std::string_view type = "text/plain; charset=utf-8")
    {
        if (state == State::closed || write_pending) return;
        state = State::writing;
        response.emplace(static_cast<http::status>(status), 11);
        response->set(http::field::content_type, type);
        response->set(http::field::cache_control, "no-store");
        response->keep_alive(false);
        if (status == 503) response->set(http::field::retry_after, "30");
        if (status == 405) response->set(http::field::allow, "GET");
        if (!head) response->body() = std::move(body);
        response->prepare_payload();
        stream.expires_after(options.write_timeout);
        write_pending = true;
        http::async_write(stream, *response, [self = shared_from_this()](Error, std::size_t) {
            self->write_pending = false;
            self->response.reset(); // Beast has finished borrowing message storage.
            self->submission.reset();
            self->close();
        });
    }

    void close()
    {
        if (state == State::closed) return;
        state = State::closed;
        queue_timer.cancel();
        Error ignored;
        stream.socket().cancel(ignored);
        stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
        stream.socket().close(ignored);
        if (submission) worker.cancel(submission->handle, CancelReason::disconnected);
        if (!write_pending) {
            response.reset();
            submission.reset();
        }
        if (auto parent = owner.lock()) parent->remove(id);
    }
};

HttpServer::Impl::Connection::~Connection()
{
    --state->count;
    if (auto parent = state->owner.lock(); parent && !parent->stopping && !parent->context.stopped()) {
        const auto weak = state->owner;
        net::post(parent->context, [weak] { if (auto owner = weak.lock()) owner->accept(); });
    }
}

void HttpServer::Impl::accept()
{
    if (stopping || accepting || connections->count >= options.maximum_connections) return;
    if (std::chrono::steady_clock::now() < retry_after) {
        if (!retry_pending) {
            retry_pending = true;
            retry.expires_at(retry_after);
            retry.async_wait([self = shared_from_this()](Error error) {
                self->retry_pending = false;
                if (!error) self->accept();
            });
        }
        return;
    }
    auto session = std::make_shared<Session>(shared_from_this(), next_id++);
    accepting = true;
    acceptor.async_accept(session->stream.socket(), [self = shared_from_this(), session](Error error) {
        self->accepting = false;
        if (!error && !self->stopping) {
            self->live.emplace(session->id, session);
            session->start();
        } else if (error && error != net::error::operation_aborted) {
            self->retry_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        }
        self->accept();
    });
}

void HttpServer::Impl::stop(bool drain)
{
    stopping = true;
    Error ignored;
    acceptor.close(ignored);
    retry.cancel();
    for (auto it = live.begin(); it != live.end();) {
        const auto session = (it++)->second; // close() erases the current entry.
        if (!drain || session->state == Session::State::reading) session->close();
    }
}

HttpServer::HttpServer(net::io_context& context, RequestWorker& worker, HttpOptions options, HealthProvider health)
    : impl_(std::make_shared<Impl>(context, worker, std::move(options), std::move(health)))
{
    if (!impl_->health) impl_->health = [&worker] {
        const auto status = worker.status();
        const auto ready = status.accepting && worker_healthy(status, std::chrono::steady_clock::now());
        return HealthReply{ready, std::format(R"({{"ready":{},"revision":{},"queued":{},"unfinished":{}}})",
            ready, json_quote(status.revision), status.queued, status.unfinished)};
    };
}

HttpServer::~HttpServer() { impl_->stop(false); }

Result<std::uint16_t> HttpServer::start()
{
    const auto& options = impl_->options;
    if (options.maximum_connections <= 0 || options.header_bytes < 128
        || !std::in_range<std::uint32_t>(options.header_bytes) || options.target_bytes <= 0
        || options.read_timeout <= std::chrono::milliseconds::zero()
        || options.write_timeout <= std::chrono::milliseconds::zero())
        return std::unexpected("invalid HTTP limits");
    Error error;
    const auto address = net::ip::make_address(options.address, error);
    if (error || !address.is_loopback()) return std::unexpected("HTTP listener must use a loopback address");
    try {
        impl_->acceptor.open(address.is_v4() ? tcp::v4() : tcp::v6());
        impl_->acceptor.set_option(net::socket_base::reuse_address(true));
        impl_->acceptor.bind({address, options.port});
        impl_->acceptor.listen(128);
        impl_->connections->owner = impl_;
        impl_->accept();
        return impl_->acceptor.local_endpoint().port();
    } catch (const boost::system::system_error& error) { return std::unexpected(error.what()); }
}

void HttpServer::stop(bool drain) { impl_->stop(drain); }

std::ptrdiff_t HttpServer::sessions() const { return impl_->connections->count; }

} // namespace updater
