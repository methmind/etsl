//
// Created by sexey on 23.05.2026.
//
module;
#include <concepts>

#include <etl/expected.h>

export module etsl.reactor:trait;

import etsl.net;
import etsl.timer;

export namespace etsl
{
    template<typename T>
    concept ReactorTrait = requires(T t, socket_t fd, typename T::operation_t& operation, typename T::dispose_operation_t& disposable, C_Timer& timer, const timer_duration_t& duration)
    {
        { t.initialize() } noexcept -> std::same_as<etl::expected<void, int32_t>>;

        { t.run() } noexcept -> std::same_as<etl::expected<void, int32_t>>;

        { t.shutdown() } noexcept -> std::same_as<etl::expected<void, int32_t>>;

        { t.associate(fd) } noexcept -> std::same_as<etl::expected<void, int32_t>>;

        { t.detach(disposable) } noexcept -> std::same_as<void>;

        { t.post(operation) } noexcept -> std::same_as<etl::expected<void, int32_t>>;

        { t.schedule(timer, duration) } noexcept -> std::same_as<void>;

        { t.unschedule(timer) } noexcept -> std::same_as<void>;
    };
}
