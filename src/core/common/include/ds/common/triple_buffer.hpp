// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

/// @file
/// @brief Lock-free triple buffer for publishing state from one thread to another (§3.3).
/// @ingroup common

namespace ds::common {

/// @brief Single-producer / single-consumer triple buffer.
/// @details The producer fills writeBuffer() and calls publish(); the consumer calls update()
/// and reads readBuffer(). Neither side ever blocks or waits for the other, and the consumer
/// always sees the most recently published, complete value (older ones are skipped).
/// Exactly one producer thread and one consumer thread may use an instance.
template <typename T>
class TripleBuffer {
public:
    /// @brief Starts with three default-constructed values.
    TripleBuffer() = default;
    /// @brief Starts with three copies of @p initial.
    explicit TripleBuffer(const T& initial) : buffers_{initial, initial, initial} {}

    /// @brief Buffer owned by the producer (filled before publish()).
    [[nodiscard]] T& writeBuffer() noexcept { return buffers_[write_]; }

    /// @brief Makes the write buffer available to the consumer; the producer gets a free
    /// buffer to write next (its content is stale and must be overwritten).
    void publish() noexcept {
        const std::uint32_t previous =
            middle_.exchange(write_ | kFreshBit, std::memory_order_acq_rel);
        write_ = previous & kIndexMask;
    }

    /// @brief Takes the latest published value, if there is a new one.
    /// @return True if readBuffer() changed.
    bool update() noexcept {
        if ((middle_.load(std::memory_order_relaxed) & kFreshBit) == 0) {
            return false;
        }
        const std::uint32_t previous = middle_.exchange(read_, std::memory_order_acq_rel);
        read_ = previous & kIndexMask;
        return true;
    }

    /// @brief Buffer owned by the consumer: the value taken by the last update().
    [[nodiscard]] const T& readBuffer() const noexcept { return buffers_[read_]; }

private:
    static constexpr std::uint32_t kIndexMask = 0x3U;
    static constexpr std::uint32_t kFreshBit = 0x4U;

    std::array<T, 3> buffers_{};
    std::uint32_t write_ = 0;               // Producer only.
    std::uint32_t read_ = 1;                // Consumer only.
    std::atomic<std::uint32_t> middle_{2};  // Shared: index + "fresh" flag.
};

}  // namespace ds::common
