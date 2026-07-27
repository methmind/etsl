//
// Created by sexey on 23.05.2026.
//
module;
#include <concepts>

#include "etl/expected.h"

export module reactor.trait;

import socket.types;

import timer;

export namespace etsl
{
    template<typename T>
    concept ReactorTrait = requires(T t, socket_t fd, typename T::task_t& task, typename T::dispose_operation_t& disposable, C_Timer& timer)
    {
        { t.initialize() } noexcept -> std::same_as<etl::expected<void, int32_t>>;

        { t.run() } noexcept -> std::same_as<void>;

        { t.shutdown() } noexcept -> std::same_as<void>;

        { t.associate(fd) } noexcept -> std::same_as<etl::expected<void, int32_t>>;

        { t.detach(disposable) } noexcept -> std::same_as<void>;

        { t.post(task) } noexcept -> std::same_as<etl::expected<void, int32_t>>;

        { t.addTimer(timer) } noexcept -> std::same_as<void>;

        { t.removeTimer(timer) } noexcept -> std::same_as<void>;
    };
}
