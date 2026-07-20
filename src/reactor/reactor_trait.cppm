//
// Created by sexey on 23.05.2026.
//
module;
#include <concepts>

#include "etl/expected.h"

export module reactor.trait;

import socket.types;
import socket.events;

export namespace etsl
{
    template<typename T>
    concept ReactorTrait = requires(T t, socket_t fd, const socket_event_callback_t& cb,
        typename T::reg_info_t& reg, typename T::task_t* task)
    {
        { t.initialize() } noexcept -> std::same_as<etl::expected<void, int32_t>>;

        { t.run() } noexcept -> std::same_as<void>;

        { t.shutdown() } noexcept -> std::same_as<void>;

        { t.attach(fd, cb, reg) } noexcept -> std::same_as<etl::expected<void, int32_t>>;

        { t.detach(fd, reg) } noexcept -> std::same_as<etl::expected<void, int32_t>>;

        { t.post(task) } noexcept -> std::same_as<etl::expected<void, int32_t>>;
    };
}
