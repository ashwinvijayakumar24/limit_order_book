// test_object_pool.cpp
// Unit tests for ObjectPool component

#include <gtest/gtest.h>
#include "../include/ObjectPool.hpp"
#include "../include/Order.hpp"
#include "../include/Types.hpp"

using namespace apexlob;

class ObjectPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool_ = std::make_unique<ObjectPool<Order, 100>>();
    }

    std::unique_ptr<ObjectPool<Order, 100>> pool_;
};

TEST_F(ObjectPoolTest, InitialState) {
    EXPECT_EQ(pool_->capacity(), 100);
    EXPECT_EQ(pool_->allocated(), 0);
    EXPECT_EQ(pool_->available(), 100);
    EXPECT_TRUE(pool_->is_empty());
    EXPECT_FALSE(pool_->is_full());
}

TEST_F(ObjectPoolTest, AcquireRelease) {
    Order* order = pool_->acquire();
    ASSERT_NE(order, nullptr);

    EXPECT_EQ(pool_->allocated(), 1);
    EXPECT_EQ(pool_->available(), 99);
    EXPECT_FALSE(pool_->is_empty());

    pool_->release(order);

    EXPECT_EQ(pool_->allocated(), 0);
    EXPECT_EQ(pool_->available(), 100);
    EXPECT_TRUE(pool_->is_empty());
}

TEST_F(ObjectPoolTest, MultipleAcquire) {
    std::vector<Order*> orders;

    // Acquire all orders
    for (size_t i = 0; i < 100; ++i) {
        Order* order = pool_->acquire();
        ASSERT_NE(order, nullptr);
        orders.push_back(order);
    }

    EXPECT_EQ(pool_->allocated(), 100);
    EXPECT_TRUE(pool_->is_full());
    EXPECT_EQ(pool_->available(), 0);

    // Try to acquire one more (should fail)
    Order* extra = pool_->acquire();
    EXPECT_EQ(extra, nullptr);

    // Release all
    for (Order* order : orders) {
        pool_->release(order);
    }

    EXPECT_EQ(pool_->allocated(), 0);
    EXPECT_EQ(pool_->available(), 100);
}

TEST_F(ObjectPoolTest, PoolGuard) {
    {
        Order* raw_order = pool_->acquire();
        ASSERT_NE(raw_order, nullptr);
        EXPECT_EQ(pool_->allocated(), 1);

        PoolGuard guard(*pool_, raw_order);
        EXPECT_EQ(guard.get(), raw_order);
        EXPECT_TRUE(guard);

        // Guard should release on destruction
    }

    EXPECT_EQ(pool_->allocated(), 0);
}

TEST_F(ObjectPoolTest, PoolGuardMove) {
    Order* order1 = pool_->acquire();
    Order* order2 = pool_->acquire();

    {
        PoolGuard guard1(*pool_, order1);
        PoolGuard guard2(*pool_, order2);

        EXPECT_EQ(pool_->allocated(), 2);

        // Move guard2 to guard1
        guard1 = std::move(guard2);

        // guard2 should be empty now
        EXPECT_FALSE(guard2);
        EXPECT_EQ(pool_->allocated(), 1);  // order2 released, order1 still held
    }

    // guard1 releases order1
    EXPECT_EQ(pool_->allocated(), 0);
}

TEST_F(ObjectPoolTest, AcquiredOrderIsDefaultConstructed) {
    Order* order = pool_->acquire();
    ASSERT_NE(order, nullptr);

    // Verify default state
    EXPECT_EQ(order->id, INVALID_ORDER_ID);
    EXPECT_EQ(order->quantity, 0);
    EXPECT_EQ(order->filled_quantity, 0);
    EXPECT_EQ(order->prev_, nullptr);
    EXPECT_EQ(order->next_, nullptr);

    pool_->release(order);
}

TEST_F(ObjectPoolTest, ReleasedOrderIsReset) {
    Order* order = pool_->acquire();
    ASSERT_NE(order, nullptr);

    // Modify order
    order->id = 123;
    order->quantity = 100;
    order->price = Price{50.0};

    pool_->release(order);

    // Re-acquire and verify it's reset
    Order* order2 = pool_->acquire();
    ASSERT_NE(order2, nullptr);
    EXPECT_EQ(order2->id, INVALID_ORDER_ID);
    EXPECT_EQ(order2->quantity, 0);

    pool_->release(order2);
}
