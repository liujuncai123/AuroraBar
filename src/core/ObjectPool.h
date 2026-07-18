/**
 * @file ObjectPool.h
 * @brief 通用对象池模板
 * @date 2026-07-06
 * @details 预分配连续内存，acquire/release O(1)，零堆分配。
 *          适用于粒子、临时缓冲区等高频创建/销毁场景。
 *          // 安全：池满返回 nullptr，调用方必须判空。
 *          // 性能：std::vector 保证连续存储，指针运算 O(1)。
 * @note 非线程安全——仅限单线程使用。
 */

#pragma once

#include <vector>
#include <cassert>
#include <cstddef>

template<typename T>
class ObjectPool {
public:
    /**
     * @brief 创建池，预分配所有对象
     * @param capacity 对象数量上限
     * @note 构造时一次性分配连续内存，后续无堆分配
     */
    explicit ObjectPool(size_t capacity) {
        m_pool.resize(capacity);  // 连续存储，默认构造所有对象
        m_freeIndices.reserve(capacity);
        for (size_t i = 0; i < capacity; ++i) {
            m_freeIndices.push_back(i);
        }
    }

    /**
     * @brief 获取一个空闲对象
     * @return 对象指针；池满返回 nullptr
     */
    T* acquire() {
        if (m_freeIndices.empty()) return nullptr;
        size_t idx = m_freeIndices.back();
        m_freeIndices.pop_back();
        return &m_pool[idx];
    }

    /**
     * @brief 归还对象
     * @param ptr 之前通过 acquire() 获取的指针
     * @pre ptr 不为 null，且确属此池
     */
    void release(T* ptr) {
        assert(ptr != nullptr);
        auto idx = static_cast<size_t>(ptr - m_pool.data());
        assert(idx < m_pool.size());
        m_freeIndices.push_back(idx);
    }

    /// @brief 池中对象总数
    size_t capacity() const { return m_pool.size(); }

    /// @brief 当前空闲对象数
    size_t available() const { return m_freeIndices.size(); }

    /// @brief 池是否满
    bool isFull() const { return m_freeIndices.empty(); }

    /// @brief 获取所有对象的数组 (连续内存，适合 OpenGL 批量上传)
    T* data() { return m_pool.data(); }
    const T* data() const { return m_pool.data(); }

private:
    std::vector<T> m_pool;               ///< 连续存储
    std::vector<size_t> m_freeIndices;   ///< 空闲索引栈
};
