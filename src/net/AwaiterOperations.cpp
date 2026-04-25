#include "uvent/net/AwaiterOperations.h"

#include <cstdio>
#include "uvent/system/SystemContext.h"

namespace usub::uvent::net::detail {

    AwaiterRead::AwaiterRead(SocketHeader* header) : header_(header) {}

    bool AwaiterRead::await_ready() {
        const bool p = this->header_->consume_read_pending();
        std::fprintf(stderr, "[await_read] await_ready fd=%d pending_was=%d\n", this->header_->fd, (int)p);
        std::fflush(stderr);
        return p;
    }

    void AwaiterRead::await_suspend(std::coroutine_handle<> h) {
        auto c =
            std::coroutine_handle<uvent::detail::AwaitableFrameBase>::from_address(h.address());

        this->header_->first = c;
        this->header_->clear_busy();

        const bool p = this->header_->consume_read_pending();
        std::fprintf(stderr, "[await_read] await_suspend fd=%d post_pending=%d\n", this->header_->fd, (int)p);
        std::fflush(stderr);
        if (p) {
            auto resumed = std::exchange(this->header_->first, nullptr);
            if (resumed) {
                system::this_thread::detail::q->enqueue(resumed);
            }
        }
    }

    void AwaiterRead::await_resume() {}

    AwaiterWrite::AwaiterWrite(SocketHeader* header) : header_(header) {}

    bool AwaiterWrite::await_ready() {
        return this->header_->consume_write_pending();
    }

    void AwaiterWrite::await_suspend(std::coroutine_handle<> h) {
        auto c =
            std::coroutine_handle<uvent::detail::AwaitableFrameBase>::from_address(h.address());

        this->header_->second = c;
        this->header_->clear_busy();

        if (this->header_->consume_write_pending()) {
            auto resumed = std::exchange(this->header_->second, nullptr);
            if (resumed) {
                system::this_thread::detail::q->enqueue(resumed);
            }
        }
    }

    void AwaiterWrite::await_resume() {}

    AwaiterAccept::AwaiterAccept(SocketHeader* header) : header_(header) {}

    bool AwaiterAccept::await_ready() {
        return this->header_->consume_read_pending();
    }

    void AwaiterAccept::await_suspend(std::coroutine_handle<> h) {
        auto c =
            std::coroutine_handle<uvent::detail::AwaitableFrameBase>::from_address(h.address());

        this->header_->first = c;
        this->header_->clear_busy();

        if (this->header_->consume_read_pending()) {
            auto resumed = std::exchange(this->header_->first, nullptr);
            if (resumed) {
                system::this_thread::detail::q->enqueue(resumed);
            }
        }
    }

    void AwaiterAccept::await_resume() {}

}  // namespace usub::uvent::net::detail
