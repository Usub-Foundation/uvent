#ifndef UVENT_SYNC_ASYNC_CHANNEL_H
#define UVENT_SYNC_ASYNC_CHANNEL_H

#include <atomic>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "uvent/base/Traits.h"
#include "uvent/sync/SyncCommon.h"
#include "uvent/sync/Wait.h"
#include "uvent/sync/WaitList.h"
#include "uvent/system/Settings.h"
#include "uvent/tasks/AwaitableFrame.h"
#include "uvent/utils/datastructures/queue/ConcurrentQueues.h"

namespace usub::uvent::sync
{
    namespace detail
    {
        inline void wake_one_waiter(WaitList& list) noexcept
        {
            notify_fence();
            if (list.empty_relaxed())
                return;
            list.lock();
            while (Waiter* w = list.pop_front_locked())
                if (fire_waiter(w))
                    break;
            list.unlock();
        }

        inline void wake_all_waiters(WaitList& list) noexcept
        {
            notify_fence();
            if (list.empty_relaxed())
                return;
            list.lock();
            while (Waiter* w = list.pop_front_locked())
                fire_waiter(w);
            list.unlock();
        }
    } // namespace detail

    template <class Channel>
    struct ChannelRecvOp
    {
        Channel* ch;

        using result_type = std::optional<typename Channel::value_type>;

        static const char* wait_reason() noexcept { return "channel.recv"; }

        bool try_complete(result_type& out)
        {
            typename Channel::value_type tmp;
            if (ch->try_recv(tmp))
            {
                out = std::move(tmp);
                return true;
            }
            if (ch->is_closed() && ch->empty_relaxed())
            {
                out = std::nullopt;
                return true;
            }
            return false;
        }

        bool attach(Waiter* w) noexcept { return ch->attach_recv(w); }

        bool detach(Waiter* w) noexcept { return ch->detach_recv(w); }

        void finalize() noexcept {}
    };

    template <class Channel>
    struct ChannelSendOp
    {
        Channel* ch;
        const typename Channel::value_type* value;

        using result_type = bool;

        static const char* wait_reason() noexcept { return "channel.send"; }

        bool try_complete(result_type& out)
        {
            if (ch->is_closed())
            {
                out = false;
                return true;
            }
            if (ch->try_send_tuple(*this->value))
            {
                out = true;
                return true;
            }
            return false;
        }

        bool attach(Waiter* w) noexcept { return ch->attach_send(w); }

        bool detach(Waiter* w) noexcept { return ch->detach_send(w); }

        void finalize() noexcept {}
    };

    template <class... Ts>
    class AsyncChannel
    {
    public:
        using value_type = std::tuple<std::decay_t<Ts>...>;

        static_assert((!is_thread_affine_v<std::decay_t<Ts>> && ...),
                      "AsyncChannel payload must not be a thread-affine type (socket, timer)");

    private:
        using Queue = usub::queue::concurrent::MPMCQueue<value_type>;

        Queue queue_;
        WaitList recv_waiters_;
        WaitList send_waiters_;
        std::atomic<bool> closed_{false};

    public:
        explicit AsyncChannel(std::size_t capacity_pow2 = 1024) : queue_(capacity_pow2) {}

        AsyncChannel(const AsyncChannel&) = delete;
        AsyncChannel& operator=(const AsyncChannel&) = delete;

        AsyncChannel(AsyncChannel&&) = delete;
        AsyncChannel& operator=(AsyncChannel&&) = delete;

        bool is_closed() const noexcept { return closed_.load(std::memory_order_acquire); }

        void close() noexcept
        {
            if (closed_.exchange(true, std::memory_order_seq_cst))
                return;
            detail::wake_all_waiters(this->recv_waiters_);
            detail::wake_all_waiters(this->send_waiters_);
        }

        [[nodiscard]] std::size_t capacity() const noexcept { return queue_.capacity(); }
        [[nodiscard]] std::size_t size_relaxed() const noexcept { return queue_.size_relaxed(); }
        [[nodiscard]] bool empty_relaxed() const noexcept { return queue_.empty_relaxed(); }

        bool attach_recv(Waiter* w) noexcept
        {
            this->recv_waiters_.lock();
            this->recv_waiters_.push_locked(w);
            this->recv_waiters_.unlock();
            detail::notify_fence();
            return this->queue_.empty_relaxed() && !this->is_closed();
        }

        bool detach_recv(Waiter* w) noexcept { return this->recv_waiters_.remove(w); }

        bool attach_send(Waiter* w) noexcept
        {
            this->send_waiters_.lock();
            this->send_waiters_.push_locked(w);
            this->send_waiters_.unlock();
            detail::notify_fence();
            return this->queue_.size_relaxed() >= this->queue_.capacity() && !this->is_closed();
        }

        bool detach_send(Waiter* w) noexcept { return this->send_waiters_.remove(w); }

        template <class... Us>
        bool try_send(Us&&... vs)
        {
            static_assert(sizeof...(Ts) == sizeof...(Us), "try_send: argument count mismatch");
            if (is_closed())
                return false;
            value_type v{std::forward<Us>(vs)...};
            if (!queue_.try_enqueue(v))
                return false;
            detail::wake_one_waiter(this->recv_waiters_);
            return true;
        }

        bool try_send_tuple(const value_type& v)
        {
            if (is_closed())
                return false;
            if (!queue_.try_enqueue(v))
                return false;
            detail::wake_one_waiter(this->recv_waiters_);
            return true;
        }

        bool try_send_tuple(value_type&& v)
        {
            if (is_closed())
                return false;
            if (!queue_.try_enqueue(std::move(v)))
                return false;
            detail::wake_one_waiter(this->recv_waiters_);
            return true;
        }

        bool try_recv(value_type& out)
        {
            if (!queue_.try_dequeue(out))
                return false;
            detail::wake_one_waiter(this->send_waiters_);
            return true;
        }

        template <class... Us>
        bool try_recv_into(Us&... out)
        {
            static_assert(sizeof...(Ts) == sizeof...(Us), "try_recv_into: argument count mismatch");
            value_type tmp;
            if (!this->try_recv(tmp))
                return false;
            assign_from_tuple(tmp, out...);
            return true;
        }

        [[nodiscard]] ChannelRecvOp<AsyncChannel> recv_op() noexcept { return ChannelRecvOp<AsyncChannel>{this}; }

        [[nodiscard]] ChannelSendOp<AsyncChannel> send_op(const value_type& v) noexcept
        {
            return ChannelSendOp<AsyncChannel>{this, &v};
        }

        template <class... Us>
        task::Awaitable<bool> send(Us&&... vs)
        {
            static_assert(sizeof...(Ts) == sizeof...(Us), "send: argument count mismatch");
            value_type v{std::forward<Us>(vs)...};
            ChannelSendOp<AsyncChannel> op{this, &v};
            for (;;)
            {
                bool sent = false;
                if (op.try_complete(sent))
                {
                    if (!system::coop::consume())
                        co_await system::this_coroutine::yield();
                    co_return sent;
                }
                OpWait<ChannelSendOp<AsyncChannel>> w{&op};
                if (!co_await w)
                    co_return false;
            }
        }

        task::Awaitable<bool> send_tuple(value_type v)
        {
            ChannelSendOp<AsyncChannel> op{this, &v};
            for (;;)
            {
                bool sent = false;
                if (op.try_complete(sent))
                {
                    if (!system::coop::consume())
                        co_await system::this_coroutine::yield();
                    co_return sent;
                }
                OpWait<ChannelSendOp<AsyncChannel>> w{&op};
                if (!co_await w)
                    co_return false;
            }
        }

        task::Awaitable<std::optional<value_type>> recv()
        {
            ChannelRecvOp<AsyncChannel> op{this};
            for (;;)
            {
                std::optional<value_type> r;
                if (op.try_complete(r))
                {
                    if (!system::coop::consume())
                        co_await system::this_coroutine::yield();
                    co_return r;
                }
                OpWait<ChannelRecvOp<AsyncChannel>> w{&op};
                if (!co_await w)
                    co_return std::nullopt;
            }
        }

        template <class... Us>
        task::Awaitable<bool> recv_into(Us&... out)
        {
            static_assert(sizeof...(Ts) == sizeof...(Us), "recv_into: argument count mismatch");
            ChannelRecvOp<AsyncChannel> op{this};
            for (;;)
            {
                std::optional<value_type> r;
                if (op.try_complete(r))
                {
                    if (!system::coop::consume())
                        co_await system::this_coroutine::yield();
                    if (!r)
                        co_return false;
                    assign_from_tuple(*r, out...);
                    co_return true;
                }
                OpWait<ChannelRecvOp<AsyncChannel>> w{&op};
                if (!co_await w)
                    co_return false;
            }
        }

    private:
        template <class Tup, class... Us, std::size_t... Is>
        static void assign_from_tuple_impl(Tup& t, std::tuple<Us&...> refs, std::index_sequence<Is...>)
        {
            ((std::get<Is>(refs) = std::move(std::get<Is>(t))), ...);
        }

        template <class... Us>
        static void assign_from_tuple(value_type& t, Us&... out)
        {
            std::tuple<Us&...> refs{out...};
            assign_from_tuple_impl(t, refs, std::index_sequence_for<Us...>{});
        }
    };

    template <class T>
    inline task::Awaitable<bool> operator<<(AsyncChannel<T>& ch, T v)
    {
        co_return co_await ch.send(std::move(v));
    }

    template <class... Ts, class... Us>
    inline task::Awaitable<bool> operator<<(AsyncChannel<Ts...>& ch, std::tuple<Us...> v)
    {
        static_assert(sizeof...(Ts) == sizeof...(Us), "operator<<: tuple size mismatch");
        co_return co_await ch.send_tuple(std::move(v));
    }

    namespace detail
    {
        template <class C>
        using channel_value_t = typename C::value_type;

        template <class C0, class... Cs>
        constexpr bool all_same_value_type_v = (std::is_same_v<channel_value_t<C0>, channel_value_t<Cs>> && ...);

        template <std::size_t N>
        struct MultiRecvWait
        {
            void* channels[N];
            bool (*attach_fns[N])(void*, Waiter*);
            bool (*detach_fns[N])(void*, Waiter*);
            bool active[N];
            Waiter nodes[N];
            std::atomic<int32_t> winner{-1};
            bool own_cancel{false};

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> h) noexcept
            {
                auto* f = &uvent::detail::frame_of(h);
                if (f->cancel_requested())
                {
                    this->own_cancel = true;
                    return false;
                }
                const int tid = current_thread_id();
                bool wait_needed = true;
                for (std::size_t i = 0; i < N; ++i)
                {
                    if (!this->active[i])
                        continue;
                    this->nodes[i].reset(h, tid);
                    this->nodes[i].winner = &this->winner;
                    this->nodes[i].index = static_cast<int32_t>(i);
                    if (!this->attach_fns[i](this->channels[i], &this->nodes[i]))
                    {
                        wait_needed = false;
                        break;
                    }
                }
                if (!wait_needed)
                {
                    this->detach_all();
                    return this->winner.load(std::memory_order_acquire) != -1;
                }
                if (f->arm_cancel(&MultiRecvWait::on_cancel, this, "channel.select"))
                {
                    int32_t expected = -1;
                    if (this->winner.compare_exchange_strong(expected, Waiter::kCancelledIndex,
                                                             std::memory_order_acq_rel, std::memory_order_relaxed))
                    {
                        f->disarm_cancel();
                        this->detach_all();
                        this->own_cancel = true;
                        return false;
                    }
                }
                return true;
            }

            bool await_resume() noexcept
            {
                if (this->own_cancel)
                    return false;
                this->detach_all();
                return this->winner.load(std::memory_order_relaxed) != Waiter::kCancelledIndex;
            }

            void detach_all() noexcept
            {
                for (std::size_t i = 0; i < N; ++i)
                    if (this->active[i])
                        this->detach_fns[i](this->channels[i], &this->nodes[i]);
            }

            static void on_cancel(uvent::detail::AwaitableFrameBase* f, void* arg) noexcept
            {
                auto* s = static_cast<MultiRecvWait*>(arg);
                int32_t expected = -1;
                if (s->winner.compare_exchange_strong(expected, Waiter::kCancelledIndex, std::memory_order_acq_rel,
                                                      std::memory_order_relaxed))
                    system::this_thread::detail::q.enqueue(f->get_coroutine_handle());
            }
        };
    } // namespace detail

    template <class Channel0, class... Channels>
    task::Awaitable<std::optional<std::pair<std::size_t, typename Channel0::value_type>>> select_recv(Channel0& c0,
                                                                                                      Channels&... cs)
    {
        static_assert(sizeof...(Channels) >= 1, "select_recv: need at least 2 channels");
        static_assert(detail::all_same_value_type_v<Channel0, Channels...>,
                      "select_recv: all channels must have the same value_type");

        using value_type = typename Channel0::value_type;
        using Result = std::optional<std::pair<std::size_t, value_type>>;
        constexpr std::size_t N = 1 + sizeof...(Channels);

        struct Vt
        {
            void* ch;
            bool (*try_recv)(void*, value_type&);
            bool (*drained)(void*);
            bool (*attach)(void*, Waiter*);
            bool (*detach)(void*, Waiter*);
        };

        auto make_vt = []<class C>(C& c) -> Vt
        {
            return Vt{&c, +[](void* p, value_type& out) { return static_cast<C*>(p)->try_recv(out); },
                      +[](void* p)
                      {
                          auto* ch = static_cast<C*>(p);
                          return ch->is_closed() && ch->empty_relaxed();
                      },
                      +[](void* p, Waiter* w) { return static_cast<C*>(p)->attach_recv(w); },
                      +[](void* p, Waiter* w) { return static_cast<C*>(p)->detach_recv(w); }};
        };

        Vt vts[N] = {make_vt(c0), make_vt(cs)...};

        for (;;)
        {
            const std::size_t start = detail::select_rotation() % N;
            for (std::size_t k = 0; k < N; ++k)
            {
                const std::size_t i = (start + k) % N;
                value_type v{};
                if (vts[i].try_recv(vts[i].ch, v))
                {
                    if (!system::coop::consume())
                        co_await system::this_coroutine::yield();
                    co_return Result{std::make_pair(i, std::move(v))};
                }
            }

            bool all_drained = true;
            bool alive[N];
            for (std::size_t i = 0; i < N; ++i)
            {
                alive[i] = !vts[i].drained(vts[i].ch);
                if (alive[i])
                    all_drained = false;
            }
            if (all_drained)
                co_return std::nullopt;

            detail::MultiRecvWait<N> waiter{};
            for (std::size_t i = 0; i < N; ++i)
            {
                waiter.channels[i] = vts[i].ch;
                waiter.attach_fns[i] = vts[i].attach;
                waiter.detach_fns[i] = vts[i].detach;
                waiter.active[i] = alive[i];
            }
            if (!co_await waiter)
                co_return std::nullopt;
        }
    }

} // namespace usub::uvent::sync

#endif // UVENT_SYNC_ASYNC_CHANNEL_H
