// test_ring_buffer.cpp
// Unit tests for SPSCRingBuffer component

#include <gtest/gtest.h>
#include "../include/SPSCRingBuffer.hpp"
#include "../include/Types.hpp"
#include <thread>
#include <vector>
#include <atomic>

using namespace apexlob;

class SPSCRingBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer_ = std::make_unique<SPSCRingBuffer<int, 1024>>();
    }

    std::unique_ptr<SPSCRingBuffer<int, 1024>> buffer_;
};

TEST_F(SPSCRingBufferTest, InitialState) {
    EXPECT_TRUE(buffer_->is_empty());
    EXPECT_FALSE(buffer_->is_full());
    EXPECT_EQ(buffer_->size(), 0);
    EXPECT_EQ(buffer_->capacity(), 1023);  // One slot reserved
}

TEST_F(SPSCRingBufferTest, PushPop) {
    const int value = 42;
    EXPECT_TRUE(buffer_->try_push(value));

    EXPECT_FALSE(buffer_->is_empty());
    EXPECT_EQ(buffer_->size(), 1);

    auto result = buffer_->try_pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, value);

    EXPECT_TRUE(buffer_->is_empty());
}

TEST_F(SPSCRingBufferTest, MultiplePushPop) {
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(buffer_->try_push(i));
    }

    EXPECT_EQ(buffer_->size(), 100);

    for (int i = 0; i < 100; ++i) {
        auto result = buffer_->try_pop();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, i);
    }

    EXPECT_TRUE(buffer_->is_empty());
}

TEST_F(SPSCRingBufferTest, FullBuffer) {
    // Fill buffer to capacity
    for (size_t i = 0; i < buffer_->capacity(); ++i) {
        EXPECT_TRUE(buffer_->try_push(static_cast<int>(i)));
    }

    EXPECT_TRUE(buffer_->is_full());
    EXPECT_FALSE(buffer_->try_push(999));  // Should fail

    // Pop one
    auto result = buffer_->try_pop();
    ASSERT_TRUE(result.has_value());

    EXPECT_FALSE(buffer_->is_full());
    EXPECT_TRUE(buffer_->try_push(999));  // Should succeed now
}

TEST_F(SPSCRingBufferTest, TryPopWithReference) {
    buffer_->try_push(42);

    int value = 0;
    EXPECT_TRUE(buffer_->try_pop(value));
    EXPECT_EQ(value, 42);

    EXPECT_FALSE(buffer_->try_pop(value));  // Empty now
}

TEST_F(SPSCRingBufferTest, Peek) {
    buffer_->try_push(42);

    const int* ptr = buffer_->peek();
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(*ptr, 42);

    // Peek again (should still be there)
    ptr = buffer_->peek();
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(*ptr, 42);

    // Pop it
    auto result = buffer_->try_pop();
    ASSERT_TRUE(result.has_value());

    // Peek should return null now
    EXPECT_EQ(buffer_->peek(), nullptr);
}

TEST_F(SPSCRingBufferTest, TryEmplace) {
    struct TestStruct {
        int a;
        int b;
        TestStruct() : a(0), b(0) {}
        TestStruct(int x, int y) : a(x), b(y) {}
    };

    SPSCRingBuffer<TestStruct, 1024> struct_buffer;

    EXPECT_TRUE(struct_buffer.try_emplace(10, 20));

    auto result = struct_buffer.try_pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->a, 10);
    EXPECT_EQ(result->b, 20);
}

TEST_F(SPSCRingBufferTest, RingWrapAround) {
    // Fill and empty partially to test wrap-around
    for (int i = 0; i < 500; ++i) {
        buffer_->try_push(i);
    }

    // Pop half
    for (int i = 0; i < 250; ++i) {
        auto result = buffer_->try_pop();
        ASSERT_TRUE(result.has_value());
    }

    // Fill again (should wrap around)
    for (int i = 500; i < 750; ++i) {
        EXPECT_TRUE(buffer_->try_push(i));
    }

    // Verify all values
    for (int i = 250; i < 750; ++i) {
        auto result = buffer_->try_pop();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, i);
    }
}

// Thread safety test (basic)
TEST_F(SPSCRingBufferTest, SingleProducerSingleConsumer) {
    SPSCRingBuffer<int, 8192> thread_buffer;
    std::atomic<bool> start{false};
    std::atomic<size_t> produced{0};
    std::atomic<size_t> consumed{0};

    const size_t num_items = 10000;

    // Producer thread
    std::thread producer([&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }

        for (size_t i = 0; i < num_items; ++i) {
            while (!thread_buffer.try_push(static_cast<int>(i))) {
                std::this_thread::yield();
            }
            produced.fetch_add(1);
        }
    });

    // Consumer thread
    std::thread consumer([&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }

        size_t count = 0;
        while (count < num_items) {
            auto result = thread_buffer.try_pop();
            if (result.has_value()) {
                EXPECT_EQ(*result, static_cast<int>(count));
                ++count;
                consumed.fetch_add(1);
            } else {
                std::this_thread::yield();
            }
        }
    });

    start.store(true);

    producer.join();
    consumer.join();

    EXPECT_EQ(produced.load(), num_items);
    EXPECT_EQ(consumed.load(), num_items);
    EXPECT_TRUE(thread_buffer.is_empty());
}

TEST_F(SPSCRingBufferTest, BatchOperations) {
    SPSCRingBuffer<int, 1024> batch_buffer;
    std::vector<int> input = {1, 2, 3, 4, 5};
    std::vector<int> output;

    // Batch push
    size_t pushed = SPSCBatchOps<int, 1024>::batch_push(
        batch_buffer, input.begin(), input.end()
    );
    EXPECT_EQ(pushed, 5);

    // Batch pop
    output.resize(10);
    size_t popped = SPSCBatchOps<int, 1024>::batch_pop(
        batch_buffer, output.begin(), 10
    );
    EXPECT_EQ(popped, 5);

    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(output[i], static_cast<int>(i + 1));
    }
}
