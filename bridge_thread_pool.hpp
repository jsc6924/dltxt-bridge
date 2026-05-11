#pragma once

#include <boost/asio/thread_pool.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace bridge_runtime {

class SharedThreadPool {
    std::mutex mutex_;
    std::unique_ptr<boost::asio::thread_pool> pool_;

public:
    void initialize(std::size_t thread_count = std::max<std::size_t>(2, std::thread::hardware_concurrency())) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pool_) {
            return;
        }

        pool_ = std::make_unique<boost::asio::thread_pool>(std::max<std::size_t>(2, thread_count));
    }

    boost::asio::thread_pool& get() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pool_) {
            throw std::runtime_error("Shared thread pool has not been initialized");
        }

        return *pool_;
    }

    bool is_initialized() {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<bool>(pool_);
    }

    void shutdown() {
        std::unique_ptr<boost::asio::thread_pool> pool;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pool = std::move(pool_);
        }

        if (!pool) {
            return;
        }

        pool->join();
    }
};

inline SharedThreadPool& shared_thread_pool_state() {
    static SharedThreadPool state;
    return state;
}

inline void initialize_shared_thread_pool(std::size_t thread_count = std::max<std::size_t>(2, std::thread::hardware_concurrency() / 4)) {
    shared_thread_pool_state().initialize(thread_count);
}

inline boost::asio::thread_pool& shared_thread_pool() {
    return shared_thread_pool_state().get();
}

inline bool shared_thread_pool_initialized() {
    return shared_thread_pool_state().is_initialized();
}

inline void shutdown_shared_thread_pool() {
    shared_thread_pool_state().shutdown();
}

}  // namespace bridge_runtime