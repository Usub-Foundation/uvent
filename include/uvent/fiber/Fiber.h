//
// Created by kirill on 12/09/26.
//

#ifndef UVENT_FIBER_FIBER_H
#define UVENT_FIBER_FIBER_H

#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "uvent/fiber/Context.h"
#include "uvent/system/Settings.h"
#include "uvent/system/StackGuard.h"
#include "uvent/system/SystemContext.h"
#include "uvent/tasks/Task.h"

/// \file
/// \brief Stackful fibers on top of the uvent coroutine runtime.
///
/// A fiber is ordinary blocking-style code running on its own stack. It is
/// hosted by a regular `task::Awaitable`, so everything that works for a
/// coroutine (spawn, JoinHandle, TaskScope, cancellation, introspection)
/// works for a fiber too, and the scheduler is not aware of fibers at all.
///
/// \code
/// task::spawn(fiber::run([&] {
///     auto n = fiber::await(sock.async_read(buf, sizeof buf));
///     fiber::await(system::this_coroutine::sleep_for(1s));
///     legacy_deep_call_stack();           // no co_await needed anywhere
/// }));
/// \endcode
///
/// Rules:
///   * `fiber::await` may only be called from inside a fiber body (any call
///     depth). Outside of one it throws `std::logic_error`.
///   * Do not call `fiber::await` inside a `catch` handler or a destructor:
///     the C++ runtime keeps per-thread exception state which is not
///     fiber-aware (coroutines forbid `co_await` there for the same reason).
///   * Stack overflow hits a guard page and terminates the process; size the
///     stack via `Options::stack_size` / `settings::fiber_stack_size`.
namespace usub::uvent::fiber
{
    struct Options
    {
        /// Usable stack bytes; 0 means settings::fiber_stack_size.
        std::size_t stack_size{0};
    };

    /// \brief Thrown inside a fiber whose host task is destroyed while the
    /// fiber is suspended in `fiber::await`. Lets RAII objects on the fiber
    /// stack run their destructors. Never swallow it.
    struct ForcedUnwind
    {
    };

    namespace detail
    {
        using HostHandle = std::coroutine_handle<uvent::detail::AwaitableFrameBase>;

        /// Type-erased awaiter protocol used by the host coroutine.
        struct StepVTable
        {
            bool (*ready)(void*) noexcept;
            std::coroutine_handle<> (*suspend)(void*, HostHandle) noexcept;
            void (*resume)(void*) noexcept;
        };

        class FiberBase
        {
        public:
            FiberBase(const FiberBase&) = delete;
            FiberBase& operator=(const FiberBase&) = delete;

            /// \brief Fiber whose body is executing on the calling thread, or nullptr.
            static FiberBase* current() noexcept;

            /// \brief Allocates the stack and prepares the first entry.
            void start(std::size_t stack_size);

            /// \brief Host side: runs the fiber until it awaits or finishes.
            void switch_in() noexcept;

            /// \brief Fiber side: hands control back to the host.
            void switch_out() noexcept;

            [[nodiscard]] bool finished() const noexcept { return this->finished_; }

            [[nodiscard]] bool started() const noexcept { return this->ctx_.created(); }

            [[nodiscard]] bool unwind_requested() const noexcept { return this->unwind_; }

            void set_step(void* obj, const StepVTable* vt) noexcept
            {
                this->step_obj_ = obj;
                this->step_vt_ = vt;
            }

            bool step_ready() noexcept { return this->step_vt_->ready(this->step_obj_); }

            std::coroutine_handle<> step_suspend(HostHandle h) noexcept
            {
                return this->step_vt_->suspend(this->step_obj_, h);
            }

            void step_resume() noexcept { this->step_vt_->resume(this->step_obj_); }

        protected:
            FiberBase() = default;

            ~FiberBase() = default;

            /// \brief Runs on the fiber stack. Must not throw.
            virtual void run_body() noexcept = 0;

            /// \brief Called from the derived destructor: if the fiber is
            /// suspended, drives it to completion with ForcedUnwind.
            void unwind_if_suspended() noexcept;

        private:
            static void entry(void* self) noexcept;

            Context ctx_;
            void* step_obj_{nullptr};
            const StepVTable* step_vt_{nullptr};
            bool finished_{false};
            bool unwind_{false};
            std::uintptr_t saved_stack_base_{0};
            FiberBase* saved_current_{nullptr};
        };

        /// Awaiter the host coroutine uses; forwards to the erased step.
        struct StepAwaiter
        {
            FiberBase* f;

            bool await_ready() const noexcept { return this->f->step_ready(); }

            template <class P>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept
            {
                return this->f->step_suspend(HostHandle::from_address(h.address()));
            }

            void await_resume() const noexcept { this->f->step_resume(); }
        };

        template <class A>
        concept HasMemberCoAwait = requires(A&& a) { std::forward<A>(a).operator co_await(); };

        template <class A>
        concept HasFreeCoAwait = requires(A&& a) { operator co_await(std::forward<A>(a)); };

        template <class A>
        decltype(auto) get_awaiter(A&& a)
        {
            if constexpr (HasMemberCoAwait<A>)
                return std::forward<A>(a).operator co_await();
            else if constexpr (HasFreeCoAwait<A>)
                return operator co_await(std::forward<A>(a));
            else
                return std::forward<A>(a);
        }

        /// Result slot: value, reference or void.
        template <class R>
        struct Slot
        {
            alignas(R) unsigned char buf[sizeof(R)];
            bool has{false};

            template <class F>
            void fill(F&& f)
            {
                ::new (static_cast<void*>(this->buf)) R(std::forward<F>(f)());
                this->has = true;
            }

            R take() { return std::move(*std::launder(reinterpret_cast<R*>(this->buf))); }

            ~Slot()
            {
                if (this->has)
                    std::launder(reinterpret_cast<R*>(this->buf))->~R();
            }
        };

        template <class R>
        struct Slot<R&>
        {
            R* p{nullptr};

            template <class F>
            void fill(F&& f)
            {
                this->p = std::addressof(std::forward<F>(f)());
            }

            R& take() { return *this->p; }
        };

        template <class R>
        struct Slot<R&&>
        {
            R* p{nullptr};

            template <class F>
            void fill(F&& f)
            {
                R&& r = std::forward<F>(f)();
                this->p = std::addressof(r);
            }

            R&& take() { return std::move(*this->p); }
        };

        template <>
        struct Slot<void>
        {
            template <class F>
            void fill(F&& f)
            {
                std::forward<F>(f)();
            }

            void take() {}
        };

        template <class Aw, class R>
        struct Step
        {
            Aw* aw;
            std::exception_ptr exc{nullptr};
            Slot<R> slot{};

            explicit Step(Aw* a) noexcept : aw(a) {}

            static bool ready(void* p) noexcept
            {
                auto* s = static_cast<Step*>(p);
                try
                {
                    return s->aw->await_ready();
                }
                catch (...)
                {
                    s->exc = std::current_exception();
                    return true;
                }
            }

            static std::coroutine_handle<> suspend(void* p, HostHandle h) noexcept
            {
                auto* s = static_cast<Step*>(p);
                try
                {
                    using S = decltype(s->aw->await_suspend(h));
                    if constexpr (std::is_void_v<S>)
                    {
                        s->aw->await_suspend(h);
                        return std::noop_coroutine();
                    }
                    else if constexpr (std::is_same_v<S, bool>)
                    {
                        return s->aw->await_suspend(h) ? std::coroutine_handle<>(std::noop_coroutine())
                                                       : std::coroutine_handle<>(h);
                    }
                    else
                        return s->aw->await_suspend(h);
                }
                catch (...)
                {
                    s->exc = std::current_exception();
                    return h;
                }
            }

            static void resume(void* p) noexcept
            {
                auto* s = static_cast<Step*>(p);
                if (s->exc)
                    return;
                try
                {
                    s->slot.fill([s]() -> decltype(auto) { return s->aw->await_resume(); });
                }
                catch (...)
                {
                    s->exc = std::current_exception();
                }
            }

            R take()
            {
                if (this->exc)
                    std::rethrow_exception(this->exc);
                return this->slot.take();
            }

            static constexpr StepVTable vtable{&Step::ready, &Step::suspend, &Step::resume};
        };

        template <class F, class V>
        class FiberTask final : public FiberBase
        {
        public:
            explicit FiberTask(F&& body) : body_(std::move(body)) {}

            ~FiberTask() { this->unwind_if_suspended(); }

            V take()
            {
                if (this->exc_)
                    std::rethrow_exception(this->exc_);
                if constexpr (!std::is_void_v<V>)
                    return std::move(*this->result_);
            }

        private:
            void run_body() noexcept override
            {
                try
                {
                    if constexpr (std::is_void_v<V>)
                        this->body_();
                    else
                        this->result_.emplace(this->body_());
                }
                catch (const ForcedUnwind&)
                {
                }
                catch (...)
                {
                    this->exc_ = std::current_exception();
                }
            }

            struct Empty
            {
            };

            F body_;
            std::conditional_t<std::is_void_v<V>, Empty, std::optional<std::conditional_t<std::is_void_v<V>, int, V>>>
                result_{};
            std::exception_ptr exc_{nullptr};
        };
    } // namespace detail

    /// \brief True when called from inside a fiber body.
    inline bool in_fiber() noexcept { return detail::FiberBase::current() != nullptr; }

    /// \brief Blocks the current fiber on any uvent awaitable (an
    /// `Awaitable<T>`, a `JoinHandle`, a socket / timer / mutex awaiter, ...)
    /// and returns what `co_await` would have returned. Exceptions propagate.
    template <class A>
    decltype(auto) await(A&& awaitable)
    {
        auto* f = detail::FiberBase::current();
        if (!f) [[unlikely]]
            throw std::logic_error("uvent::fiber::await called outside of a fiber");
        if (f->unwind_requested()) [[unlikely]]
            throw ForcedUnwind{};

        auto&& aw = detail::get_awaiter(std::forward<A>(awaitable));
        using Aw = std::remove_cvref_t<decltype(aw)>;
        using R = decltype(aw.await_resume());
        using StepT = detail::Step<Aw, R>;

        StepT step(std::addressof(aw));
        f->set_step(&step, &StepT::vtable);
        f->switch_out();
        f->set_step(nullptr, nullptr);

        if (f->unwind_requested()) [[unlikely]]
            throw ForcedUnwind{};
        return step.take();
    }

    /// \brief Gives other tasks on this worker a chance to run.
    inline void yield() { await(system::this_coroutine::yield()); }

    /// \brief Runs `body` on a dedicated stack. The returned Awaitable is an
    /// ordinary uvent task: `co_await` it, `task::spawn` it, put it into a
    /// TaskScope. Its result / exception is that of `body`.
    template <class F>
    task::Awaitable<std::invoke_result_t<F&>> run(F body, Options opt = {})
    {
        using V = std::invoke_result_t<F&>;
        detail::FiberTask<F, V> t(std::move(body));
        t.start(opt.stack_size ? opt.stack_size : settings::fiber_stack_size);
        for (;;)
        {
            t.switch_in();
            if (t.finished())
                break;
            co_await detail::StepAwaiter{&t};
        }
        co_return t.take();
    }
} // namespace usub::uvent::fiber

#endif // UVENT_FIBER_FIBER_H
