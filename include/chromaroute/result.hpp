#pragma once

#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace chromaroute {

enum class ErrorCode {
    unknown_format,
    unknown_space,
    unknown_transfer_function,
    invalid_argument,
    unsupported_conversion,
    buffer_too_small,
    size_overflow,
    overlapping_buffers,
};

struct Error {
    ErrorCode code;
    std::string message;
};

// Value-or-error result shaped like std::expected (which C++20 lacks).
// Documented failures are reported through Result/Status, never by throwing.
// Calling value() on an error, or error() on a value, is a contract violation
// and terminates the program.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return checkedGet<0>(storage_); }
    [[nodiscard]] const T& value() const& { return checkedGet<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::move(checkedGet<0>(storage_)); }
    [[nodiscard]] const Error& error() const { return checkedGet<1>(storage_); }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

private:
    template <std::size_t Index, typename Storage>
    static auto& checkedGet(Storage& storage)
    {
        auto* alternative = std::get_if<Index>(&storage);
        if (alternative == nullptr) {
            std::terminate();
        }
        return *alternative;
    }

    std::variant<T, Error> storage_;
};

// Result without a value: success, or an Error.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const Error& error() const
    {
        if (!error_) {
            std::terminate();
        }
        return *error_;
    }

private:
    std::optional<Error> error_;
};

using Status = Result<void>;

} // namespace chromaroute
