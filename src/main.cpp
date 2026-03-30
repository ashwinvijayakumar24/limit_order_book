// main.cpp
// ApexLOB - Ultra-low-latency Limit Order Book System
// TCP server with thread affinity and lock-free queues

#include "../include/MatchingEngine.hpp"
#include "../include/WireProtocol.hpp"
#include "../include/SPSCRingBuffer.hpp"
#include "../include/Order.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <csignal>
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>
#include <iostream>
#include <chrono>
#include <optional>
#include <array>
#include <span>

namespace apexlob {

// ============================================================================
// THREAD AFFINITY UTILITIES
// ============================================================================

void set_thread_affinity([[maybe_unused]] int cpu_core) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_core, &cpuset);

    pthread_t thread = pthread_self();
    int rc = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Warning: Failed to set thread affinity to core " << cpu_core
                  << ": " << strerror(rc) << std::endl;
    } else {
        std::cout << "Thread pinned to CPU core " << cpu_core << std::endl;
    }
#else
    // Thread affinity is not supported on this platform
    std::cout << "Thread affinity not supported on this platform" << std::endl;
#endif
}

// ============================================================================
// TCP SOCKET WRAPPER
// ============================================================================

class TCPSocket {
public:
    explicit TCPSocket(int fd) : fd_(fd) {
        // Set TCP_NODELAY for low latency
        int flag = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        // Set socket to non-blocking
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }

    ~TCPSocket() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    // Non-copyable, movable
    TCPSocket(const TCPSocket&) = delete;
    TCPSocket& operator=(const TCPSocket&) = delete;
    TCPSocket(TCPSocket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }
    TCPSocket& operator=(TCPSocket&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }

    ssize_t read(void* buf, size_t count) const {
        return ::read(fd_, buf, count);
    }

    ssize_t write(const void* buf, size_t count) const {
        return ::write(fd_, buf, count);
    }

private:
    int fd_;
};

// ============================================================================
// TCP SERVER
// ============================================================================

class TCPServer {
public:
    TCPServer(uint16_t port) : port_(port), listen_fd_(-1) {}

    ~TCPServer() {
        if (listen_fd_ >= 0) {
            close(listen_fd_);
        }
    }

    bool bind_and_listen() {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
            return false;
        }

        // Set socket options
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // Set listen socket non-blocking so accept() returns on SIGINT
        int flags = fcntl(listen_fd_, F_GETFL, 0);
        fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "Failed to bind to port " << port_ << ": " << strerror(errno) << std::endl;
            return false;
        }

        if (::listen(listen_fd_, 10) < 0) {
            std::cerr << "Failed to listen: " << strerror(errno) << std::endl;
            return false;
        }

        std::cout << "TCP server listening on port " << port_ << std::endl;
        return true;
    }

    [[nodiscard]] std::optional<TCPSocket> accept() const {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            return std::nullopt;
        }

        std::cout << "Accepted connection from " << inet_ntoa(client_addr.sin_addr)
                  << ":" << ntohs(client_addr.sin_port) << std::endl;
        return TCPSocket(client_fd);
    }

private:
    uint16_t port_;
    int listen_fd_;
};

// ============================================================================
// MESSAGE BUFFER (for TCP framing)
// ============================================================================

class MessageBuffer {
public:
    static constexpr size_t BUFFER_SIZE = 8192;

    MessageBuffer() : data_{}, pos_(0), expected_length_(0) {}

    // Read available data from socket; returns 0 on EOF/error, SIZE_MAX on EAGAIN
    size_t read_from_socket(const TCPSocket& socket) {
        size_t available = BUFFER_SIZE - pos_;
        if (available == 0) {
            // Buffer full, reset
            pos_ = 0;
            expected_length_ = 0;
            available = BUFFER_SIZE;
        }

        ssize_t n = socket.read(data_.data() + pos_, available);
        if (n > 0) {
            pos_ += static_cast<size_t>(n);
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return 0;  // Error
        }
        return static_cast<size_t>(n);
    }

    // Extract complete messages
    template<typename Callback>
    size_t extract_messages(Callback&& callback) {
        size_t extracted = 0;
        size_t offset = 0;

        while (offset + sizeof(FrameHeader) <= pos_) {
            // Read frame header
            FrameHeader frame;
            std::memcpy(&frame, data_.data() + offset, sizeof(FrameHeader));
            frame.from_network_order();

            const size_t message_length = frame.length;
            const size_t total_length = sizeof(FrameHeader) + message_length;

            if (message_length > BUFFER_SIZE || message_length == 0) {
                // Invalid message, skip
                offset += sizeof(FrameHeader);
                continue;
            }

            if (offset + total_length > pos_) {
                // Incomplete message, wait for more data
                break;
            }

            // Extract message
            std::span<const std::byte> message_span(
                reinterpret_cast<const std::byte*>(data_.data() + offset + sizeof(FrameHeader)),
                message_length
            );

            if (callback(message_span)) {
                ++extracted;
            }

            offset += total_length;
        }

        // Shift remaining data to front
        if (offset > 0) {
            const size_t remaining = pos_ - offset;
            if (remaining > 0) {
                std::memmove(data_.data(), data_.data() + offset, remaining);
            }
            pos_ = remaining;
        }

        return extracted;
    }

private:
    std::array<std::byte, BUFFER_SIZE> data_;
    size_t pos_;
    size_t expected_length_;
};

// ============================================================================
// NETWORK THREAD (receives orders, pushes to queue)
// ============================================================================

void network_thread_func(
    TCPServer& server,
    MatchingEngine<>::InputQueue& input_queue,
    MatchingEngine<>::OrderPool& order_pool,
    MatchingEngine<>::CancelQueue& cancel_queue,
    std::atomic<int>& client_fd,
    std::atomic<bool>& running
) {
    set_thread_affinity(0);  // Pin to core 0

    std::optional<TCPSocket> client_socket;
    MessageBuffer buffer;

    while (running.load(std::memory_order_acquire)) {
        // Accept new connections
        if (!client_socket.has_value()) {
            client_socket = server.accept();
            if (!client_socket.has_value()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            // Share fd with output thread
            client_fd.store(client_socket->fd(), std::memory_order_release);
        }

        // Read data from socket; returns 0 on EOF/error
        size_t bytes = buffer.read_from_socket(*client_socket);
        if (bytes == 0) {
            // Client disconnected or hard error
            client_fd.store(-1, std::memory_order_release);
            client_socket.reset();
            buffer = MessageBuffer{};
            continue;
        }

        // Extract and process messages
        buffer.extract_messages([&](std::span<const std::byte> message_span) -> bool {
            // Parse message type
            auto msg_type = MessageParser::peek_message_type(message_span);
            if (!msg_type.has_value()) {
                return false;
            }

            // Handle new order
            if (*msg_type == MessageType::NewOrder) {
                NewOrderMessage msg;
                if (MessageParser::parse_new_order(message_span, msg)) {
                    // Acquire order from pool
                    Order* order = order_pool.acquire();
                    if (order != nullptr) {
                        *order = msg.to_order();
                        if (input_queue.try_push(order)) {
                            return true;
                        } else {
                            // Queue full, release order
                            order_pool.release(order);
                        }
                    }
                }
            }
            // Handle cancel order — forward to matching thread via cancel queue
            else if (*msg_type == MessageType::CancelOrder) {
                CancelOrderMessage msg;
                if (MessageParser::parse_cancel_order(message_span, msg)) {
                    cancel_queue.try_push(msg.order_id);
                    return true;
                }
            }

            return false;
        });

        std::this_thread::sleep_for(std::chrono::microseconds(1));  // Small yield
    }
}

// ============================================================================
// MATCHING THREAD (processes orders from queue)
// ============================================================================

void matching_thread_func(
    MatchingEngine<>& engine,
    MatchingEngine<>::InputQueue& input_queue,
    MatchingEngine<>::CancelQueue& cancel_queue,
    MatchingEngine<>::OutputQueue& output_queue,
    MatchingEngine<>::TradeQueue& trade_queue,
    std::atomic<bool>& running
) {
    set_thread_affinity(1);  // Pin to core 1

    auto start_time = std::chrono::high_resolution_clock::now();
    size_t orders_processed = 0;

    while (running.load(std::memory_order_acquire)) {
        const size_t processed = engine.process_inbound_orders(
            input_queue, cancel_queue, output_queue, trade_queue);
        orders_processed += processed;

        if (processed == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(1));  // Small yield
        }

        // Print stats every second
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        if (elapsed >= 1) {
            std::cout << "Processed " << orders_processed << " orders, "
                      << "Active: " << engine.active_order_count() << std::endl;
            start_time = now;
            orders_processed = 0;
        }
    }
}

// ============================================================================
// OUTPUT THREAD (sends execution reports back to client)
// ============================================================================

void output_thread_func(
    MatchingEngine<>::OutputQueue& output_queue,
    MatchingEngine<>::TradeQueue& trade_queue,
    std::atomic<int>& client_fd,
    std::atomic<bool>& running
) {
    set_thread_affinity(2);  // Pin to core 2

    uint32_t sequence = 0;

    // Helper: write a framed message to the connected client
    auto send_framed = [&](const auto& wire_msg) {
        int fd = client_fd.load(std::memory_order_acquire);
        if (fd < 0) return;  // no client connected

        auto body = MessageEncoder::encode_array(wire_msg);

        // Write 4-byte frame header (big-endian length) then message body
        FrameHeader frame{static_cast<std::uint32_t>(body.size())};
        frame.to_network_order();
        ::write(fd, &frame, sizeof(frame));
        ::write(fd, body.data(), body.size());
    };

    while (running.load(std::memory_order_acquire)) {
        // Send execution reports
        ExecutionReport report;
        while (output_queue.try_pop(report)) {
            auto msg = ExecutionReportMessage::from_report(report, sequence++);
            send_framed(msg);
        }

        // Send trade notifications
        Trade trade;
        while (trade_queue.try_pop(trade)) {
            auto msg = TradeMessage::from_trade(trade, sequence++);
            send_framed(msg);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
}

// ============================================================================
// SIGNAL HANDLER
// ============================================================================

} // namespace apexlob

// Global pointer so the signal handler can reach the running flag
static std::atomic<bool>* g_running = nullptr;
static void shutdown_handler(int) {
    if (g_running) g_running->store(false, std::memory_order_release);
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    using namespace apexlob;

    const uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 8888;
    const Symbol symbol = 1;  // Default symbol

    std::cout << "=== ApexLOB - Ultra-Low-Latency Limit Order Book ===" << std::endl;
    std::cout << "Port: " << port << std::endl;

    // Heap-allocate all large objects — combined they exceed 27 MB which
    // overflows the default 8 MB stack on macOS/Linux if left as locals.
    auto engine       = std::make_unique<MatchingEngine<>>(symbol);
    auto input_queue  = std::make_unique<MatchingEngine<>::InputQueue>();
    auto output_queue = std::make_unique<MatchingEngine<>::OutputQueue>();
    auto trade_queue  = std::make_unique<MatchingEngine<>::TradeQueue>();

    // Cancel queue: network thread pushes OrderId, matching thread drains
    auto cancel_queue = std::make_unique<MatchingEngine<>::CancelQueue>();

    // Shared client socket fd: network thread sets on connect, output thread reads to send
    std::atomic<int> client_fd{-1};

    // Create TCP server
    TCPServer server(port);
    if (!server.bind_and_listen()) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }

    // Control flag and signal handler
    std::atomic<bool> running{true};
    g_running = &running;
    std::signal(SIGINT,  shutdown_handler);
    std::signal(SIGTERM, shutdown_handler);

    // Start threads
    std::thread network_thread(network_thread_func,
                               std::ref(server),
                               std::ref(*input_queue),
                               std::ref(engine->order_pool()),
                               std::ref(*cancel_queue),
                               std::ref(client_fd),
                               std::ref(running));

    std::thread matching_thread(matching_thread_func,
                                std::ref(*engine),
                                std::ref(*input_queue),
                                std::ref(*cancel_queue),
                                std::ref(*output_queue),
                                std::ref(*trade_queue),
                                std::ref(running));

    std::thread output_thread(output_thread_func,
                              std::ref(*output_queue),
                              std::ref(*trade_queue),
                              std::ref(client_fd),
                              std::ref(running));

    std::cout << "All threads started. Press Ctrl+C to stop." << std::endl;

    // Wait for threads to finish
    network_thread.join();
    matching_thread.join();
    output_thread.join();

    std::cout << "Shutting down..." << std::endl;
    return 0;
}
