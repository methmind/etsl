//
// Created by sexey on 19.07.2026.
//
module;
#include <type_traits>
#include <winsock2.h>

#include <etl/delegate.h>
#include <etl/expected.h>
#include <etl/intrusive_list.h>

#include <util/noncopyable.h>


export module etsl.reactor:iocp_defs;

namespace etsl
{
    export class C_OperationIOCP : private WSAOVERLAPPED
    {
    public:
        using delegate_t = etl::delegate<void(C_OperationIOCP& operation, uint32_t bytes, int32_t error)>;

        ~C_OperationIOCP() noexcept { assert(!this->inFlight_ && "UAF error caught!"); }

        C_OperationIOCP() noexcept : WSAOVERLAPPED({}), inFlight_(false) {}

        explicit C_OperationIOCP(const delegate_t& delegate) noexcept : WSAOVERLAPPED({}), delegate_(delegate), inFlight_(false) {}

        ETSL_NON_COPYABLE_NON_MOVABLE(C_OperationIOCP);

        [[nodiscard]] bool inFlight() const noexcept { return this->inFlight_; }

        void setDelegate(const delegate_t& delegate) noexcept { this->delegate_ = delegate; }

        template <typename post_t> requires std::same_as<std::invoke_result_t<post_t&, WSAOVERLAPPED*>, etl::expected<void, int32_t>>
        [[nodiscard]] etl::expected<void, int32_t> assign(post_t&& post) noexcept
        {
            if (!this->delegate_.is_valid()) {
                return etl::unexpected(WSAEINVAL);
            }

            if (this->inFlight_) {
                return etl::unexpected(WSAEALREADY);
            }

            memset(static_cast<WSAOVERLAPPED*>(this), 0, sizeof(WSAOVERLAPPED));
            if (const auto err = post(this); !err) {
                return err;
            }

            this->inFlight_ = true;
            return {};
        }

    private:
        friend class C_ReactorIOCP;

        [[nodiscard]] etl::expected<void, int32_t> complete(uint32_t bytes, int32_t error) noexcept
        {
            if (!this->inFlight_ || !this->delegate_.is_valid()) {
                assert(false && "Inconsistent operation state!");
                return etl::unexpected(WSAEINVAL);
            }

            this->inFlight_ = false;
            this->delegate_(*this, bytes, error);
            return {};
        }

        delegate_t delegate_;
        bool inFlight_;
    };

    export struct dispose_operation_iocp_s : etl::bidirectional_link<0>
    {
        etl::delegate<void()> delegate{};

        ETSL_DEFAULT_NON_COPYABLE_NON_MOVABLE(dispose_operation_iocp_s)
    };

    static_assert(std::is_base_of_v<WSAOVERLAPPED, C_OperationIOCP>,
        "C_OperationIOCP must inherit from WSAOVERLAPPED!");
}
