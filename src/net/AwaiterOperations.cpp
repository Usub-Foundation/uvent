#include "uvent/net/AwaiterOperations.h"

#include "uvent/system/SystemContext.h"

namespace usub::uvent::net::detail
{

    AwaiterRead::AwaiterRead(SocketHeader* header) : header_(header) {}

    bool AwaiterRead::await_ready() { return this->header_->is_read_armed(); }

    void AwaiterRead::await_suspend(std::coroutine_handle<> h)
    {
        auto c = std::coroutine_handle<uvent::detail::AwaitableFrameBase>::from_address(h.address());

        this->header_->first = c;
        this->header_->clear_busy();

        if (this->header_->is_read_armed())
        {
            auto resumed = std::exchange(this->header_->first, nullptr);
            if (resumed)
            {
                system::this_thread::detail::q->enqueue(resumed);
            }
        }
    }

    void AwaiterRead::await_resume() {}

    AwaiterWrite::AwaiterWrite(SocketHeader* header) : header_(header) {}

    bool AwaiterWrite::await_ready() { return this->header_->is_write_armed(); }

    void AwaiterWrite::await_suspend(std::coroutine_handle<> h)
    {
        auto c = std::coroutine_handle<uvent::detail::AwaitableFrameBase>::from_address(h.address());

        this->header_->second = c;
        this->header_->clear_busy();

        if (this->header_->is_write_armed())
        {
            auto resumed = std::exchange(this->header_->second, nullptr);
            if (resumed)
            {
                system::this_thread::detail::q->enqueue(resumed);
            }
        }
    }

    void AwaiterWrite::await_resume() {}

    AwaiterAccept::AwaiterAccept(SocketHeader* header) : header_(header) {}

    bool AwaiterAccept::await_ready() { return this->header_->is_read_armed(); }

    void AwaiterAccept::await_suspend(std::coroutine_handle<> h)
    {
        auto c = std::coroutine_handle<uvent::detail::AwaitableFrameBase>::from_address(h.address());

        this->header_->first = c;
        this->header_->clear_busy();

        if (this->header_->is_read_armed())
        {
            auto resumed = std::exchange(this->header_->first, nullptr);
            if (resumed)
            {
                system::this_thread::detail::q->enqueue(resumed);
            }
        }
    }

    void AwaiterAccept::await_resume() {}

} // namespace usub::uvent::net::detail
