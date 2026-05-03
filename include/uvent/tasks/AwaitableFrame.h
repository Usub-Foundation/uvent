//
// Created by kirill on 1/4/25.
//

#ifndef UVENT_AWAITABLEFRAME_H
#define UVENT_AWAITABLEFRAME_H

#include <atomic>
#include <coroutine>
#include <memory>
#include <new>
#include <ranges>

#include "Awaitable.h"
#include "uvent/base/Predefines.h"
#include "uvent/utils/datastructures/queue/FastQueue.h"

namespace usub::uvent
{
    namespace detail
    {
        enum DestroyingPolicy
        {
            DEFAULT,
            FORCED
        };

        struct deferred_task_tag
        {
        };

        template <class T>
        using no_cvr_t = std::remove_cv_t<std::remove_reference_t<T>>;
        template <class F>
        concept DeferredFrame = std::derived_from<no_cvr_t<F>, deferred_task_tag>;

        class AwaitableFrameBase
        {
        public:
            template <class, class>
            friend class task::Awaitable;

            AwaitableFrameBase();

            virtual ~AwaitableFrameBase() = default;

            virtual bool await_ready();

            void destroy(DestroyingPolicy policy = DEFAULT);

            void set_calling_coroutine(std::coroutine_handle<> h);

            void set_next_coroutine(std::coroutine_handle<> h);

            std::coroutine_handle<> get_calling_coroutine();

            std::coroutine_handle<> get_coroutine_handle();

            std::coroutine_handle<> get_next_coroutine();

            void resume();

            static void push_frame_into_task_queue(std::coroutine_handle<> h);

            void push_frame_to_be_destroyed();

            [[nodiscard]] int get_thread_id() const { return this->t_id_; }

            [[nodiscard]] int get_thread_id() { return this->t_id_; }

            void set_thread_id(int t_id) { this->t_id_ = t_id; }

        protected:
            std::exception_ptr exception_{nullptr};
            std::coroutine_handle<> coro_{nullptr};
            std::coroutine_handle<> prev_{nullptr};
            std::coroutine_handle<> next_{nullptr};
            int t_id_{0};
        };

        template <class T>
        class AwaitableFrame : public AwaitableFrameBase
        {
        public:
            AwaitableFrame() noexcept = default;

            ~AwaitableFrame() override;

            void unhandled_exception() { this->exception_ = std::current_exception(); }

            auto get_return_object()
            {
                using selt_t = std::remove_reference_t<decltype(*this)>;
                this->coro_ = std::coroutine_handle<AwaitableFrame>::from_promise(*this);
                return task::Awaitable<T, selt_t>{this};
            }

            void return_value(T value)
            {
                new (&this->result_) T(std::move(value));
                this->has_result_ = true;
            }

            T get()
            {
                if (this->exception_)
                    std::rethrow_exception(this->exception_);
                return std::move(*std::launder(reinterpret_cast<T*>(&this->result_)));
            }

            std::suspend_always initial_suspend() noexcept;

            std::suspend_always final_suspend() noexcept;

            std::suspend_always yield_value(T value) noexcept;

        private:
            bool has_result_ = false;
            alignas(T) unsigned char result_[sizeof(T)]{};
        };

        template <>
        class AwaitableFrame<void> : public AwaitableFrameBase
        {
        public:
            AwaitableFrame() noexcept = default;

            ~AwaitableFrame() override;

            auto get_return_object()
            {
                using self_t = std::remove_reference_t<decltype(*this)>;

                this->coro_ = std::coroutine_handle<AwaitableFrame>::from_promise(*this);
                return task::Awaitable<void, self_t>{this};
            }

            void return_void() {}

            void unhandled_exception() { this->exception_ = std::current_exception(); }

            void get()
            {
                if (this->exception_)
                    std::rethrow_exception(this->exception_);
            }

            std::suspend_always initial_suspend() noexcept;

            std::suspend_always final_suspend() noexcept;

            std::suspend_always yield_value() noexcept;
        };

        template <typename T>
        class AwaitableIOFrame : public AwaitableFrameBase, public deferred_task_tag
        {
        public:
            AwaitableIOFrame() noexcept = default;

            ~AwaitableIOFrame() override;

            void unhandled_exception() { this->exception_ = std::current_exception(); }

            auto get_return_object()
            {
                using selt_t = std::remove_reference_t<decltype(*this)>;
                this->coro_ = std::coroutine_handle<AwaitableIOFrame>::from_promise(*this);
                return task::Awaitable<T, selt_t>{this};
            }

            void return_value(T value)
            {
                new (&this->result_) T(std::move(value));
                this->has_result_ = true;
            }

            T get()
            {
                if (this->exception_)
                    std::rethrow_exception(this->exception_);
                return std::move(*std::launder(reinterpret_cast<T*>(&this->result_)));
            }

            std::suspend_never initial_suspend() noexcept;

            std::suspend_always final_suspend() noexcept;

        private:
            bool has_result_ = false;
            alignas(T) unsigned char result_[sizeof(T)]{};
        };

        template <typename T>
        std::suspend_always AwaitableIOFrame<T>::final_suspend() noexcept
        {
#if UVENT_DEBUG
            spdlog::trace("Entering final_suspend for coroutine {}", this->coro_.address());
#endif
            if (this->prev_)
            {
                auto prev = std::exchange(this->prev_, nullptr);
                auto parent = std::coroutine_handle<AwaitableFrameBase>::from_address(prev.address());
                AwaitableFrameBase::push_frame_into_task_queue(static_cast<std::coroutine_handle<>>(parent));
            }
            this->push_frame_to_be_destroyed();
            return {};
        }

        template <typename T>
        std::suspend_never AwaitableIOFrame<T>::initial_suspend() noexcept
        {
            return {};
        }

        template <class T>
        std::suspend_always AwaitableFrame<T>::initial_suspend() noexcept
        {
            return {};
        }

        template <class T>
        std::suspend_always AwaitableFrame<T>::final_suspend() noexcept
        {
#if UVENT_DEBUG
            spdlog::trace("Entering final_suspend for coroutine {}", this->coro_.address());
#endif
            if (this->prev_)
            {
                auto c_temp = std::coroutine_handle<::usub::uvent::detail::AwaitableFrameBase>::from_address(
                    std::exchange(this->prev_, nullptr).address());
                push_frame_into_task_queue(static_cast<std::coroutine_handle<>>(c_temp));
            }
            this->push_frame_to_be_destroyed();
            return {};
        }

        template <class T>
        std::suspend_always AwaitableFrame<T>::yield_value(T value) noexcept
        {
            new (&this->result_) T(std::move(value));
            this->has_result_ = true;

            if (this->prev_)
            {
                auto parent = std::coroutine_handle<::usub::uvent::detail::AwaitableFrameBase>::from_address(
                    this->prev_.address());
                push_frame_into_task_queue(static_cast<std::coroutine_handle<>>(parent));
            }
            return {};
        }

        template <class T>
        AwaitableFrame<T>::~AwaitableFrame()
        {
#if UVENT_DEBUG
            spdlog::trace("Destroying coroutine {}", this->coro_.address());
#endif
            if (this->has_result_)
                std::launder(reinterpret_cast<T*>(&this->result_))->~T();
        }

        template <class T>
        AwaitableIOFrame<T>::~AwaitableIOFrame()
        {
#if UVENT_DEBUG
            spdlog::info("Destroying coroutine IO {}", this->coro_.address());
#endif
            if (this->has_result_)
                std::launder(reinterpret_cast<T*>(&this->result_))->~T();
        }
    } // namespace detail

    namespace system::this_thread::detail
    {
        /// \brief Thread local task queue.
        thread_local extern std::unique_ptr<queue::single_thread::Queue<std::coroutine_handle<>>> q;
    } // namespace system::this_thread::detail

    namespace task
    {
        template <class FrameType>
        template <class U>
        void Awaitable<void, FrameType>::await_suspend(std::coroutine_handle<U> h)
        {
            auto ph = std::coroutine_handle<detail::AwaitableFrameBase>::from_address(h.address());
            auto& p = ph.promise();

            auto child = this->frame_->get_coroutine_handle();
            p.set_next_coroutine(child);
            this->frame_->set_calling_coroutine(h);

            if constexpr (!detail::DeferredFrame<FrameType>)
            {
                if (child && !child.done())
                    detail::AwaitableFrameBase::push_frame_into_task_queue(static_cast<std::coroutine_handle<>>(child));
            }
        }

        template <class Value, class FrameType>
        template <class U>
        void Awaitable<Value, FrameType>::await_suspend(std::coroutine_handle<U> h)
        {
            auto& p = std::coroutine_handle<detail::AwaitableFrameBase>::from_address(h.address()).promise();

            auto child = this->frame_->get_coroutine_handle();
            p.set_next_coroutine(child);
            this->frame_->set_calling_coroutine(h);

            if constexpr (!detail::DeferredFrame<FrameType>)
            {
                if (child && !child.done())
                {
                    detail::AwaitableFrameBase::push_frame_into_task_queue(static_cast<std::coroutine_handle<>>(child));
                }
            }
        }

        template <class FrameType>
        void Awaitable<void, FrameType>::await_resume()
        {
            this->frame_->get();
        }

        template <class FrameType>
        Awaitable<void, FrameType>::Awaitable(promise_type* af) : frame_(af)
        {
        }

        template <class FrameType>
        typename Awaitable<void, FrameType>::promise_type* Awaitable<void, FrameType>::get_promise()
        {
            return this->frame_;
        }

        template <class FrameType>
        bool Awaitable<void, FrameType>::await_ready() const noexcept
        {
            return !frame_ || frame_->get_coroutine_handle().done();
        }
    } // namespace task
} // namespace usub::uvent

#endif // UVENT_AWAITABLEFRAME_H
