//
// Created by sexey on 23.05.2026.
//
module;
#include <concepts>

#include "etl/expected.h"

export module event_loop.trait;

export namespace etsl
{
    template<typename T>
    concept EventLoopTrait = requires(T t)
    {
        { t.initialize() } noexcept -> std::same_as<etl::expected<void, int32_t>>;
        { t.run() } noexcept -> std::same_as<void>;
        { t.shutdown() } noexcept -> std::same_as<void>;
    };
}
