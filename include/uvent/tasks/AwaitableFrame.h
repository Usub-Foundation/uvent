//
// Created by kirill on 1/4/25.
//

#ifndef UVENT_AWAITABLEFRAME_H
#define UVENT_AWAITABLEFRAME_H

#include <atomic>
#include <coroutine>
#include <memory>
#include <cstdlib>
#include <new>
#include <ranges>

#include "Awaitable.h"
#include "uvent/base/Predefines.h"
#include "uvent/sync/CancelState.h"
#include "uvent/system/StackGuard.h"
#include "uvent/utils/datastructures/queue/FastQueue.h"
#include "uvent/utils/datastructures/queue/IntrusiveMPSC.h"

namespace usub::uvent
{
    namespace task
    {
        struct TaskStateBase;
    }

    namespace introspection::detail
    {
        struct Shard;
    }

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

        struct local_frame_tag
        {
        };

        template <class T>
        using no_cvr_t = std::remove_cv_t<std::remove_reference_t<T>>;
        template <class F>
        concept DeferredFrame = std::derived_from<no_cvr_t<F>, deferred_task_tag>;
        template <class F>
        concept LocalFrame = std::derived_from<no_cvr_t<F>, local_frame_tag>;

        /// Alignment of every coroutine frame. C++20 hands promise_type::operator
        /// new only the size, so a frame holding an over-aligned local (channels
        /// and queues carry alignas(CACHELINE_SIZE) members) would otherwise come
        /// out of a 16-byte-aligned allocation: a misaligned object, and a crash
        /// once the compiler emits aligned vector stores for it (-march with AVX).
        inline constexpr std::align_val_t kFrameAlign{64};

        /// Aligned frame memory on top of plain malloc: over-allocate, align, and
        /// keep the raw pointer in the word before the block. glibc serves
        /// aligned operator new outside malloc's thread cache (~135 ns vs ~47 ns
        /// for a fresh lazy frame, measured), while this stays on the fast path.
        inline void* frame_alloc(std::size_t sz)
        {
            constexpr std::size_t align = static_cast<std::size_t>(kFrameAlign);
            void* raw = std::malloc(sz + align + sizeof(void*));
            if (!raw) [[unlikely]]
                throw std::bad_alloc();
            const auto base = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
            const auto aligned = (base + align - 1) & ~static_cast<std::uintptr_t>(align - 1);
            reinterpret_cast<void**>(aligned)[-1] = raw;
            return reinterpret_cast<void*>(aligned);
        }
        inline void frame_free(void* p) noexcept
        {
            if (p)
                std::free(static_cast<void**>(p)[-1]);
        }

        /// Per-thread free-list allocator for coroutine frames: size classes of
        /// kClass bytes up to kMaxSize, every block allocated kFrameAlign-aligned.
        /// A block freed on another worker simply joins that worker's list.
        /// Allocation is a pointer pop, which is why lazy frames go through it as
        /// well: an aligned operator new bypasses malloc's thread cache and costs
        /// ~3x a plain malloc (measured 47 -> 135 ns per co_await of a lazy child).
        class IOFramePool
        {
            static constexpr std::size_t kClass = 64;
            static constexpr std::size_t kMaxSize = 1024;
            static constexpr std::size_t kClasses = kMaxSize / kClass;
            static constexpr std::size_t kMaxCached = 4096; // per class; excess goes back to malloc

            struct Node
            {
                Node* next;
            };

            Node* heads_[kClasses]{};
            std::size_t counts_[kClasses]{};

            static constexpr std::size_t class_of(std::size_t sz) noexcept { return (sz + kClass - 1) / kClass; }

        public:
            static IOFramePool& local() noexcept
            {
                thread_local IOFramePool pool;
                return pool;
            }

            void* allocate(std::size_t sz)
            {
                const std::size_t cls = class_of(sz);
                if (cls == 0 || cls > kClasses)
                    return frame_alloc(sz);
                Node*& head = this->heads_[cls - 1];
                if (head)
                {
                    Node* n = head;
                    head = n->next;
                    --this->counts_[cls - 1];
                    return n;
                }
                static_assert(kClass % static_cast<std::size_t>(kFrameAlign) == 0,
                              "pool classes must keep every block kFrameAlign-aligned");
                return frame_alloc(cls * kClass);
            }

            void deallocate(void* p, std::size_t sz) noexcept
            {
                const std::size_t cls = class_of(sz);
                if (cls == 0 || cls > kClasses || this->counts_[cls - 1] >= kMaxCached)
                {
                    frame_free(p);
                    return;
                }
                auto* n = static_cast<Node*>(p);
                n->next = this->heads_[cls - 1];
                this->heads_[cls - 1] = n;
                ++this->counts_[cls - 1];
            }

            ~IOFramePool()
            {
                for (Node* head : this->heads_)
                    while (head)
                    {
                        Node* n = head;
                        head = n->next;
                        frame_free(n);
                    }
            }
        };


        class AwaitableFrameBase : public queue::concurrent::MPSCNode
        {
        public:
            template <class, class>
            friend class task::Awaitable;

            AwaitableFrameBase();

            // Frames of every promise deriving from this base come from the
            // kFrameAlign-aligned per-thread pool (plain aligned new when the pool
            // is disabled). Pool blocks are individually allocated, so the unsized
            // delete below (used by clang < 19, which lacks sized deallocation by
            // default) can hand any of them straight back to the aligned global delete.
#ifndef UVENT_NO_IO_FRAME_POOL
            static void* operator new(std::size_t sz) { return IOFramePool::local().allocate(sz); }
            static void operator delete(void* p, std::size_t sz) noexcept { IOFramePool::local().deallocate(p, sz); }
#else
            static void* operator new(std::size_t sz) { return frame_alloc(sz); }
            static void operator delete(void* p, std::size_t) noexcept { frame_free(p); }
#endif
            static void operator delete(void* p) noexcept { frame_free(p); }

            bool await_ready();

            void destroy(DestroyingPolicy policy = DEFAULT);

            void set_calling_coroutine(std::coroutine_handle<> h);

            void set_next_coroutine(std::coroutine_handle<> h);

            std::coroutine_handle<> get_calling_coroutine();

            std::coroutine_handle<> get_coroutine_handle();

            std::coroutine_handle<> get_next_coroutine();

            void resume();

            static void push_frame_into_task_queue(std::coroutine_handle<> h);

            void push_frame_to_be_destroyed();

            std::coroutine_handle<> final_transfer() noexcept;

            std::coroutine_handle<> yield_transfer() noexcept;

            [[nodiscard]] int get_thread_id() const { return this->t_id_; }

            [[nodiscard]] int get_thread_id() { return this->t_id_; }

            void set_thread_id(int t_id) { this->t_id_ = t_id; }

            using cancel_fn_t = void (*)(AwaitableFrameBase*, void*);

            [[nodiscard]] sync::CancelState* cancel_state() const noexcept { return this->cancel_; }

            void set_cancel_state(sync::CancelState* s) noexcept { this->cancel_ = s; }

            [[nodiscard]] task::TaskStateBase* task_state() const noexcept { return this->task_; }

            void set_task_state(task::TaskStateBase* t) noexcept { this->task_ = t; }

            [[nodiscard]] bool cancel_requested() const noexcept
            {
                return this->cancel_ && this->cancel_->requested.load(std::memory_order_relaxed);
            }

            bool arm_cancel(cancel_fn_t fn, void* arg, const char* reason) noexcept
            {
                this->cancel_fn_ = fn;
                this->cancel_arg_ = arg;
                this->wait_reason_ = reason;
#ifdef UVENT_TASK_INTROSPECTION
                this->stamp_wait_time();
#endif
                return this->cancel_requested();
            }

            void disarm_cancel() noexcept
            {
                this->cancel_fn_ = nullptr;
                this->wait_reason_ = nullptr;
            }

            void run_cancel_hook() noexcept
            {
                if (auto fn = std::exchange(this->cancel_fn_, nullptr))
                    fn(this, this->cancel_arg_);
            }

            [[nodiscard]] bool has_cancel_hook() const noexcept { return this->cancel_fn_ != nullptr; }

            void detach_from_task() noexcept
            {
                this->cancel_ = nullptr;
                this->task_ = nullptr;
            }

            [[nodiscard]] const char* wait_reason() const noexcept { return this->wait_reason_; }

            [[nodiscard]] const char* name() const noexcept { return this->name_; }

            void set_name(const char* n) noexcept { this->name_ = n; }

            [[nodiscard]] uint64_t trace_id() const noexcept { return this->trace_id_; }

            void set_trace_id(uint64_t id) noexcept { this->trace_id_ = id; }

            [[nodiscard]] std::coroutine_handle<> prev_handle() const noexcept { return this->prev_; }

            [[nodiscard]] std::coroutine_handle<> next_handle() const noexcept { return this->next_; }

            [[nodiscard]] std::coroutine_handle<> take_pending_destroy() noexcept
            {
                return std::exchange(this->pending_destroy_, std::coroutine_handle<>{});
            }

            void on_loop_resume() noexcept
            {
                this->cancel_fn_ = nullptr;
                this->wait_reason_ = nullptr;
            }

#ifdef UVENT_TASK_INTROSPECTION
            void stamp_wait_time() noexcept;

            [[nodiscard]] uint64_t created_ns() const noexcept { return this->created_ns_; }

            [[nodiscard]] uint64_t wait_since_ns() const noexcept { return this->wait_since_ns_; }
#endif

        protected:
            ~AwaitableFrameBase();

            std::exception_ptr exception_{nullptr};
            std::coroutine_handle<> coro_{nullptr};
            std::coroutine_handle<> prev_{nullptr};
            std::coroutine_handle<> next_{nullptr};
            std::coroutine_handle<> pending_destroy_{nullptr};
            int t_id_{0};
            sync::CancelState* cancel_{nullptr};
            task::TaskStateBase* task_{nullptr};
            cancel_fn_t cancel_fn_{nullptr};
            void* cancel_arg_{nullptr};
            const char* wait_reason_{nullptr};
            const char* name_{nullptr};
            uint64_t trace_id_{0};
#ifdef UVENT_TASK_INTROSPECTION

        public:
            AwaitableFrameBase* reg_prev_{nullptr};
            AwaitableFrameBase* reg_next_{nullptr};
            introspection::detail::Shard* reg_shard_{nullptr};
            uint64_t created_ns_{0};
            uint64_t wait_since_ns_{0};
#endif
        };

        inline AwaitableFrameBase& frame_of(std::coroutine_handle<> h) noexcept
        {
            return std::coroutine_handle<AwaitableFrameBase>::from_address(h.address()).promise();
        }

        struct FinalAwaiter
        {
            bool await_ready() const noexcept { return false; }

            template <class Promise>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept
            {
                return static_cast<AwaitableFrameBase&>(h.promise()).final_transfer();
            }

            void await_resume() const noexcept {}
        };

        struct YieldAwaiter
        {
            bool await_ready() const noexcept { return false; }

            template <class Promise>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept
            {
                return static_cast<AwaitableFrameBase&>(h.promise()).yield_transfer();
            }

            void await_resume() const noexcept {}
        };

        template <class T>
        class AwaitableFrame : public AwaitableFrameBase
        {
        public:
            AwaitableFrame() noexcept = default;

            ~AwaitableFrame();

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

            FinalAwaiter final_suspend() noexcept;

            YieldAwaiter yield_value(T value) noexcept;

        private:
            bool has_result_ = false;
            alignas(T) unsigned char result_[sizeof(T)]{};
        };

        template <>
        class AwaitableFrame<void> : public AwaitableFrameBase
        {
        public:
            AwaitableFrame() noexcept = default;

            ~AwaitableFrame();

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

            FinalAwaiter final_suspend() noexcept;

            std::suspend_always yield_value() noexcept;
        };

        template <class T>
        class LocalAwaitableFrame final : public AwaitableFrame<T>, public local_frame_tag
        {
        public:
            auto get_return_object()
            {
                this->coro_ = std::coroutine_handle<LocalAwaitableFrame>::from_promise(*this);
                return task::Awaitable<T, LocalAwaitableFrame>{this};
            }
        };

        template <>
        class LocalAwaitableFrame<void> final : public AwaitableFrame<void>, public local_frame_tag
        {
        public:
            auto get_return_object()
            {
                this->coro_ = std::coroutine_handle<LocalAwaitableFrame>::from_promise(*this);
                return task::Awaitable<void, LocalAwaitableFrame>{this};
            }
        };


        template <typename T>
        class AwaitableIOFrame : public AwaitableFrameBase, public deferred_task_tag, public local_frame_tag
        {
        public:
            AwaitableIOFrame() noexcept = default;

            ~AwaitableIOFrame();

            // allocation: inherited from AwaitableFrameBase (pooled, kFrameAlign-aligned)

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

            FinalAwaiter final_suspend() noexcept;

        private:
            bool has_result_ = false;
            alignas(T) unsigned char result_[sizeof(T)]{};
        };

        template <typename T>
        FinalAwaiter AwaitableIOFrame<T>::final_suspend() noexcept
        {
#if UVENT_DEBUG
            spdlog::trace("Entering final_suspend for coroutine {}", this->coro_.address());
#endif
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
        FinalAwaiter AwaitableFrame<T>::final_suspend() noexcept
        {
#if UVENT_DEBUG
            spdlog::trace("Entering final_suspend for coroutine {}", this->coro_.address());
#endif
            return {};
        }

        template <class T>
        YieldAwaiter AwaitableFrame<T>::yield_value(T value) noexcept
        {
            new (&this->result_) T(std::move(value));
            this->has_result_ = true;
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
        thread_local extern queue::single_thread::Queue<std::coroutine_handle<>> q;
    } // namespace system::this_thread::detail

    namespace task
    {
        template <class T>
        using LocalAwaitable = Awaitable<T, detail::LocalAwaitableFrame<T>>;

        template <class FrameType>
        template <class U>
        std::coroutine_handle<> Awaitable<void, FrameType>::await_suspend(std::coroutine_handle<U> h)
        {
            auto ph = std::coroutine_handle<detail::AwaitableFrameBase>::from_address(h.address());
            auto& p = ph.promise();

            auto child = this->frame_->get_coroutine_handle();
            p.set_next_coroutine(child);
            this->frame_->set_calling_coroutine(h);
            this->frame_->set_cancel_state(p.cancel_state());
            this->frame_->set_trace_id(p.trace_id());

            if constexpr (!detail::DeferredFrame<FrameType>)
            {
                if (child && !child.done())
                {
                    if (system::stack_guard::stack_too_deep()) [[unlikely]]
                    {
                        detail::AwaitableFrameBase::push_frame_into_task_queue(child);
                        return std::noop_coroutine();
                    }
                    return child;
                }
            }
            return std::noop_coroutine();
        }

        template <class Value, class FrameType>
        template <class U>
        std::coroutine_handle<> Awaitable<Value, FrameType>::await_suspend(std::coroutine_handle<U> h)
        {
            auto& p = std::coroutine_handle<detail::AwaitableFrameBase>::from_address(h.address()).promise();

            auto child = this->frame_->get_coroutine_handle();
            p.set_next_coroutine(child);
            this->frame_->set_calling_coroutine(h);
            this->frame_->set_cancel_state(p.cancel_state());
            this->frame_->set_trace_id(p.trace_id());

            if constexpr (!detail::DeferredFrame<FrameType>)
            {
                if (child && !child.done())
                {
                    if (system::stack_guard::stack_too_deep()) [[unlikely]]
                    {
                        detail::AwaitableFrameBase::push_frame_into_task_queue(child);
                        return std::noop_coroutine();
                    }
                    return child;
                }
            }
            return std::noop_coroutine();
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
