#pragma once

#include "Types.hpp"
#include <array>
#include <atomic>
#include <optional>

namespace apexlob {

// ============================================================================
// SINGLE-PRODUCER SINGLE-CONSUMER LOCK-FREE RING BUFFER
// ============================================================================
// High-performance SPSC queue with:
// - Lock-free operations using atomic operations
// - False sharing prevention via cache-line alignment
// - Power-of-two sizing for fast modulo operations
// - Memory ordering optimizations for x86-64

template<typename T, std::size_t Capacity>
    requires TriviallyCopyable<T> && PowerOfTwo<Capacity>
class SPSCRingBuffer {
public:
    SPSCRingBuffer() noexcept
        : head_(0)
        , tail_(0)
    {}

    // Non-copyable, non-movable
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer(SPSCRingBuffer&&) = delete;
    SPSCRingBuffer& operator=(SPSCRingBuffer&&) = delete;

    // ---- PRODUCER INTERFACE (single thread only) ----

    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t current_head = head_.load(std::memory_order_relaxed);
        const std::size_t next_head = increment(current_head);

        // Check if buffer is full
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[current_head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    template<typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept {
        const std::size_t current_head = head_.load(std::memory_order_relaxed);
        const std::size_t next_head = increment(current_head);

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[current_head] = T(std::forward<Args>(args)...);
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    // ---- CONSUMER INTERFACE (single thread only) ----

    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const std::size_t current_tail = tail_.load(std::memory_order_relaxed);

        // Check if buffer is empty
        if (current_tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        T item = buffer_[current_tail];
        tail_.store(increment(current_tail), std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t current_tail = tail_.load(std::memory_order_relaxed);

        if (current_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        out = buffer_[current_tail];
        tail_.store(increment(current_tail), std::memory_order_release);
        return true;
    }

    // Peek at front without consuming
    [[nodiscard]] const T* peek() const noexcept {
        const std::size_t current_tail = tail_.load(std::memory_order_relaxed);

        if (current_tail == head_.load(std::memory_order_acquire)) {
            return nullptr;
        }

        return &buffer_[current_tail];
    }

    // ---- CAPACITY QUERIES ----

    [[nodiscard]] bool is_empty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_full() const noexcept {
        const std::size_t current_head = head_.load(std::memory_order_acquire);
        const std::size_t next_head = increment(current_head);
        return next_head == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & MASK;
    }

    [[nodiscard]] constexpr std::size_t capacity() const noexcept {
        return Capacity - 1;  // One slot reserved for full/empty distinction
    }

private:
    static constexpr std::size_t MASK = Capacity - 1;

    [[nodiscard]] static constexpr std::size_t increment(std::size_t idx) noexcept {
        return (idx + 1) & MASK;
    }

    // ---- DATA MEMBERS ----
    // Cache-line aligned to prevent false sharing

    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> head_;  // Producer writes
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> tail_;  // Consumer writes
    alignas(CACHE_LINE_SIZE) std::array<T, Capacity> buffer_;
};

// ============================================================================
// MULTI-PRODUCER SINGLE-CONSUMER (MPSC) RING BUFFER
// ============================================================================
// Similar to SPSC but with CAS operations for multiple producers

template<typename T, std::size_t Capacity>
    requires TriviallyCopyable<T> && PowerOfTwo<Capacity>
class MPSCRingBuffer {
public:
    MPSCRingBuffer() noexcept
        : head_(0)
        , tail_(0)
    {}

    MPSCRingBuffer(const MPSCRingBuffer&) = delete;
    MPSCRingBuffer& operator=(const MPSCRingBuffer&) = delete;

    // ---- PRODUCER INTERFACE (multiple threads) ----

    [[nodiscard]] bool try_push(const T& item) noexcept {
        std::size_t current_head = head_.load(std::memory_order_relaxed);

        for (;;) {
            const std::size_t next_head = increment(current_head);

            // Check if full
            if (next_head == tail_.load(std::memory_order_acquire)) {
                return false;
            }

            // Try to claim the slot
            if (head_.compare_exchange_weak(current_head, next_head,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
                buffer_[current_head] = item;
                return true;
            }
            // CAS failed, retry with updated current_head
        }
    }

    // ---- CONSUMER INTERFACE (single thread) ----

    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const std::size_t current_tail = tail_.load(std::memory_order_relaxed);

        if (current_tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        T item = buffer_[current_tail];
        tail_.store(increment(current_tail), std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool is_empty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    [[nodiscard]] constexpr std::size_t capacity() const noexcept {
        return Capacity - 1;
    }

private:
    static constexpr std::size_t MASK = Capacity - 1;

    [[nodiscard]] static constexpr std::size_t increment(std::size_t idx) noexcept {
        return (idx + 1) & MASK;
    }

    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> head_;
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> tail_;
    alignas(CACHE_LINE_SIZE) std::array<T, Capacity> buffer_;
};

// ============================================================================
// BATCH OPERATIONS (for higher throughput)
// ============================================================================

template<typename T, std::size_t Capacity>
class SPSCBatchOps {
public:
    using Buffer = SPSCRingBuffer<T, Capacity>;

    // Push multiple items at once (producer side)
    template<typename Iterator>
    static std::size_t batch_push(Buffer& buffer, Iterator begin, Iterator end) noexcept {
        std::size_t count = 0;
        for (auto it = begin; it != end; ++it) {
            if (!buffer.try_push(*it)) {
                break;
            }
            ++count;
        }
        return count;
    }

    // Pop multiple items at once (consumer side)
    template<typename OutputIterator>
    static std::size_t batch_pop(Buffer& buffer, OutputIterator out, std::size_t max_items) noexcept {
        std::size_t count = 0;
        for (std::size_t i = 0; i < max_items; ++i) {
            auto item = buffer.try_pop();
            if (!item) {
                break;
            }
            *out++ = *item;
            ++count;
        }
        return count;
    }
};

} // namespace apexlob
