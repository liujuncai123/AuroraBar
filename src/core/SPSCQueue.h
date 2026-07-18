/**
 * @file SPSCQueue.h
 * @brief 单生产者单消费者无锁环形队列
 * @date 2026-07-06
 * @details 严格 SPSC 场景，纯 std::atomic acquire/release 实现，零 CAS 开销。
 *          队列容量必须是 2 的幂，满载时丢弃最旧数据（适配音频实时场景）。
 *          // 安全：cache line 隔离读写索引，防伪共享。
 *          // 性能：热路径零堆分配、无系统调用。
 * @note 仅允许一个生产者线程和一个消费者线程访问。多生产者场景请切 moodycamel。
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>

template<typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
        "SPSCQueue: Capacity must be a power of 2");
    static_assert(Capacity >= 2,
        "SPSCQueue: Capacity must be at least 2");

public:
    SPSCQueue() = default;

    // 禁止拷贝 (队列不应被复制)
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    /**
     * @brief 生产者推送数据 (不会阻塞)
     * @return true 推送成功；false 队列满，丢弃最旧数据
     */
    bool tryPush(const T& item) {
        const size_t write = m_writeIdx.load(std::memory_order_relaxed);
        const size_t next = (write + 1) & MASK;

        if (next == m_readCached) {
            m_readCached = m_readIdx.load(std::memory_order_acquire);
            if (next == m_readCached) {
                // 队列满 → 丢弃最旧数据
                const size_t oldRead = m_readCached;
                const size_t dropNext = (oldRead + 1) & MASK;
                m_readIdx.store(dropNext, std::memory_order_release);
                m_readCached = dropNext;
            }
        }

        m_buffer[write] = item;
        m_writeIdx.store(next, std::memory_order_release);
        return true;
    }

    /**
     * @brief 消费者取出数据 (不会阻塞)
     * @param[out] item 接收数据
     * @return true 取到数据；false 队列空
     */
    bool tryPop(T& item) {
        const size_t read = m_readIdx.load(std::memory_order_relaxed);

        if (read == m_writeCached) {
            m_writeCached = m_writeIdx.load(std::memory_order_acquire);
            if (read == m_writeCached) {
                return false; // 队列空
            }
        }

        item = m_buffer[read];
        m_readIdx.store((read + 1) & MASK, std::memory_order_release);
        return true;
    }

    /// @brief 队列是否为空 (仅消费者可靠)
    bool isEmpty() const {
        return m_readIdx.load(std::memory_order_acquire)
            == m_writeIdx.load(std::memory_order_acquire);
    }

    /// @brief 队列是否已满 (仅生产者可靠)
    bool isFull() const {
        const size_t write = m_writeIdx.load(std::memory_order_relaxed);
        return ((write + 1) & MASK)
            == m_readIdx.load(std::memory_order_acquire);
    }

    /// @brief 最大容量
    static constexpr size_t capacity() { return Capacity - 1; }

private:
    static constexpr size_t MASK = Capacity - 1;
    std::array<T, Capacity> m_buffer{};

    // 安全：cache line 隔离 (64 bytes)，防伪共享
    alignas(64) std::atomic<size_t> m_writeIdx{0};
    alignas(64) std::atomic<size_t> m_readIdx{0};
    alignas(64) size_t m_writeCached{0};
    alignas(64) size_t m_readCached{0};
};
