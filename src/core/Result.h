/**
 * @file Result.h
 * @brief 统一错误处理类型 Result<T, Error>
 * @date 2026-07-06
 * @details 遵循 ADR-002：所有可能失败的操作返回 Result<T, Error>，禁止抛异常。
 *          内部使用 std::variant 存储成功值或错误信息。
 * @note 线程安全：仅用于值传递，不跨线程共享。
 */

#pragma once

#include <string>
#include <variant>

// ============================================================
// 错误码枚举
// ============================================================
enum class ErrorCode : int32_t {
    kSuccess = 0,
    kInvalidArgument = 1,
    kNotFound = 2,
    kInternalError = 3,
    kTimeout = 4,
    kAudioDeviceError = 5,
    kOpenGLError = 6,
    kConfigParseError = 7,
};

// ============================================================
// 错误信息
// ============================================================
struct Error {
    ErrorCode code = ErrorCode::kSuccess;
    std::string message;
    const char* file = nullptr;
    int line = 0;

    bool isOk() const { return code == ErrorCode::kSuccess; }
};

/// @brief 创建错误对象的宏 (自动捕获文件/行号)
#define MAKE_ERROR(c, msg) Error{c, msg, __FILE__, __LINE__}

// ============================================================
// Result<T, Error>
// ============================================================
template<typename T>
class Result {
public:
    /// @brief 创建成功结果
    static Result Ok(T value) {
        Result r;
        r.m_data = std::move(value);
        return r;
    }

    /// @brief 创建失败结果
    static Result Err(Error error) {
        Result r;
        r.m_data = std::move(error);
        return r;
    }

    /// @brief 是否成功
    bool isOk() const { return std::holds_alternative<T>(m_data); }

    /// @brief 是否失败
    bool isErr() const { return std::holds_alternative<Error>(m_data); }

    /// @brief 获取成功值 (调用前必须 isOk())
    T& value() { return std::get<T>(m_data); }
    const T& value() const { return std::get<T>(m_data); }

    /// @brief 获取成功值，失败时返回默认值
    T valueOr(const T& defaultValue) const {
        return isOk() ? std::get<T>(m_data) : defaultValue;
    }

    /// @brief 获取错误信息 (调用前必须 isErr())
    const Error& error() const { return std::get<Error>(m_data); }

private:
    Result() = default;
    std::variant<T, Error> m_data;
};

// ============================================================
// Result<void> 特化 (无返回值)
// ============================================================
template<>
class Result<void> {
public:
    static Result Ok() {
        Result r;
        r.m_isOk = true;
        return r;
    }

    static Result Err(Error error) {
        Result r;
        r.m_isOk = false;
        r.m_error = std::move(error);
        return r;
    }

    bool isOk() const { return m_isOk; }
    bool isErr() const { return !m_isOk; }
    const Error& error() const { return m_error; }

private:
    Result() = default;
    bool m_isOk = false;
    Error m_error;
};
