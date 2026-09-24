#include "uvent/net/AwaiterOperations.h"

#include "uvent/system/SystemContext.h"

namespace usub::uvent::net::detail
{
#ifdef UVENT_SOCKET_OWNER_FORWARDING
    namespace
    {
        void cancel_socket_read(uvent::detail::AwaitableFrameBase* f, void* arg) noexcept
        {
            auto* header = static_cast<SocketHeader*>(arg);
            const auto h = f->get_coroutine_handle();
            if (header->unpark_read(h))
                system::resume_waiter(h);
        }

        void cancel_socket_write(uvent::detail::AwaitableFrameBase* f, void* arg) noexcept
        {
            auto* header = static_cast<SocketHeader*>(arg);
            const auto h = f->get_coroutine_handle();
            if (header->unpark_write(h))
                system::resume_waiter(h);
        }
    } // namespace
#endif

    namespace
    {
        template <bool Read>
        UVENT_ALWAYS_INLINE_FN void suspend_on(SocketHeader* header, std::coroutine_handle<> h, const char* reason)
        {
            auto c = std::coroutine_handle<uvent::detail::AwaitableFrameBase>::from_address(h.address());
            header->clear_busy();
#ifdef UVENT_SOCKET_OWNER_FORWARDING
            auto& f = c.promise();
            const bool cancel_pending = f.arm_cancel(Read ? &cancel_socket_read : &cancel_socket_write, header, reason);
            if (cancel_pending)
            {
                // Cancellation already requested: do not park, let the coroutine observe it.
                f.disarm_cancel();
                system::this_thread::detail::q.enqueue(c);
                return;
            }
#endif
            const bool parked = Read ? header->park_read(c) : header->park_write(c);
            if (!parked)
                system::this_thread::detail::q.enqueue(c);
        }
    } // namespace

    AwaiterRead::AwaiterRead(SocketHeader* header) : header_(header) {}

    bool AwaiterRead::await_ready() { return false; }

    void AwaiterRead::await_suspend(std::coroutine_handle<> h) { suspend_on<true>(this->header_, h, "socket.read"); }

    void AwaiterRead::await_resume() {}

    AwaiterWrite::AwaiterWrite(SocketHeader* header) : header_(header) {}

    bool AwaiterWrite::await_ready() { return false; }

    void AwaiterWrite::await_suspend(std::coroutine_handle<> h) { suspend_on<false>(this->header_, h, "socket.write"); }

    void AwaiterWrite::await_resume() {}

    AwaiterAccept::AwaiterAccept(SocketHeader* header) : header_(header) {}

    bool AwaiterAccept::await_ready() { return false; }

    void AwaiterAccept::await_suspend(std::coroutine_handle<> h)
    {
        suspend_on<true>(this->header_, h, "socket.accept");
    }

    void AwaiterAccept::await_resume() {}

} // namespace usub::uvent::net::detail
