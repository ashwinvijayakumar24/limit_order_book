#pragma once

#include "Types.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <new>

namespace apexlob {

// ============================================================================
// LOCK-FREE OBJECT POOL
// ============================================================================
// Pre-allocated object pool with O(1) acquire/release operations
// Uses intrusive free-list and cache-line aligned storage
// Thread-safe for SPSC (single producer, single consumer) usage

template<typename T, std::size_t Capacity>
    requires Poolable<T> && CacheAligned<T>
class ObjectPool {
public:
    ObjectPool() noexcept {
        initialize();
    }

    ~ObjectPool() noexcept {
        // Objects are trivially destructible (required by Poolable concept)
        // Storage is automatically freed via std::array
    }

    // Non-copyable, non-movable
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    // ---- ACQUIRE / RELEASE ----

    [[nodiscard]] T* acquire() noexcept {
        T* obj = free_list_.load(std::memory_order_acquire);

        while (obj != nullptr) {
            T* next = obj->next_;
            if (free_list_.compare_exchange_weak(obj, next,
                                                  std::memory_order_release,
                                                  std::memory_order_acquire)) {
                obj->next_ = nullptr;  // Clear intrusive pointer
                allocated_count_.fetch_add(1, std::memory_order_relaxed);
                return obj;
            }
            // CAS failed, obj was updated by compare_exchange_weak
        }

        return nullptr;  // Pool exhausted
    }

    void release(T* obj) noexcept {
        if (obj == nullptr) return;

        // Reset object to default state (placement new)
        std::destroy_at(obj);
        new (obj) T();

        // Add back to free list
        T* old_head = free_list_.load(std::memory_order_relaxed);
        do {
            obj->next_ = old_head;
        } while (!free_list_.compare_exchange_weak(old_head, obj,
                                                     std::memory_order_release,
                                                     std::memory_order_relaxed));

        allocated_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    // ---- STATISTICS ----

    [[nodiscard]] std::size_t capacity() const noexcept {
        return Capacity;
    }

    [[nodiscard]] std::size_t allocated() const noexcept {
        return allocated_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t available() const noexcept {
        return Capacity - allocated();
    }

    [[nodiscard]] bool is_full() const noexcept {
        return allocated() >= Capacity;
    }

    [[nodiscard]] bool is_empty() const noexcept {
        return allocated() == 0;
    }

private:
    void initialize() noexcept {
        // Build intrusive free-list linking all objects
        for (std::size_t i = 0; i < Capacity - 1; ++i) {
            T* current = &storage_[i];
            T* next = &storage_[i + 1];
            new (current) T();  // Placement new
            current->next_ = next;
        }

        // Last object points to nullptr
        T* last = &storage_[Capacity - 1];
        new (last) T();
        last->next_ = nullptr;

        // Free list starts at first object
        free_list_.store(&storage_[0], std::memory_order_release);
        allocated_count_.store(0, std::memory_order_release);
    }

    // ---- DATA MEMBERS ----

    alignas(CACHE_LINE_SIZE) std::atomic<T*> free_list_{nullptr};
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> allocated_count_{0};
    alignas(CACHE_LINE_SIZE) std::array<T, Capacity> storage_;
};

// ============================================================================
// POOL STATISTICS (for monitoring)
// ============================================================================

template<typename T, std::size_t Capacity>
struct PoolStats {
    std::size_t capacity;
    std::size_t allocated;
    std::size_t available;
    double utilization;  // 0.0 to 1.0

    static PoolStats capture(const ObjectPool<T, Capacity>& pool) noexcept {
        const std::size_t alloc = pool.allocated();
        const std::size_t cap = pool.capacity();
        return PoolStats{
            .capacity = cap,
            .allocated = alloc,
            .available = cap - alloc,
            .utilization = static_cast<double>(alloc) / cap
        };
    }
};

// ============================================================================
// RAII POOL GUARD (automatic release)
// ============================================================================

template<typename T, std::size_t Capacity>
class PoolGuard {
public:
    explicit PoolGuard(ObjectPool<T, Capacity>& pool, T* obj) noexcept
        : pool_(pool), obj_(obj) {}

    ~PoolGuard() noexcept {
        if (obj_) {
            pool_.release(obj_);
        }
    }

    // Non-copyable
    PoolGuard(const PoolGuard&) = delete;
    PoolGuard& operator=(const PoolGuard&) = delete;

    // Movable
    PoolGuard(PoolGuard&& other) noexcept
        : pool_(other.pool_), obj_(other.obj_) {
        other.obj_ = nullptr;
    }

    PoolGuard& operator=(PoolGuard&& other) noexcept {
        if (this != &other) {
            if (obj_) {
                pool_.release(obj_);
            }
            obj_ = other.obj_;
            other.obj_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] T* get() const noexcept { return obj_; }
    [[nodiscard]] T* operator->() const noexcept { return obj_; }
    [[nodiscard]] T& operator*() const noexcept { return *obj_; }
    [[nodiscard]] explicit operator bool() const noexcept { return obj_ != nullptr; }

    T* release() noexcept {
        T* tmp = obj_;
        obj_ = nullptr;
        return tmp;
    }

private:
    ObjectPool<T, Capacity>& pool_;
    T* obj_;
};

} // namespace apexlob
