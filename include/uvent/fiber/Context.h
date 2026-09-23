//
// Created by kirill on 12/09/26.
//

#ifndef UVENT_FIBER_CONTEXT_H
#define UVENT_FIBER_CONTEXT_H

#include <cstddef>
#include <cstdint>

/// \file
/// \brief Low-level execution-context primitive backing usub::uvent::fiber.
///
/// A Context owns one dedicated native stack and can be entered / left with
/// `switch_in()` / `switch_out()`. The implementation is platform specific:
///   * Linux, *BSD, macOS (x86_64, aarch64): hand-written assembly switch
///     (see src/fiber/switch_*.S) + mmap'ed stack with a guard page.
///   * Windows (x64, ARM64): Win32 fibers (CreateFiberEx / SwitchToFiber).
///
/// ASan / TSan builds annotate every switch so the sanitizers follow the stack
/// change instead of reporting false positives.
namespace usub::uvent::fiber::detail
{
    using entry_fn_t = void (*)(void*);

    class Context
    {
    public:
        Context() = default;

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        /// \brief Frees the native stack. The context must not be running.
        ~Context();

        /// \brief Allocates a stack of at least `stack_size` usable bytes and
        /// prepares the context so that the first `switch_in()` calls
        /// `entry(arg)` on that stack.
        /// \attention `entry` must never return: it has to end with a final
        /// `switch_out()`. Returning from it is undefined behaviour.
        /// \throws std::bad_alloc when the stack cannot be reserved.
        void create(std::size_t stack_size, entry_fn_t entry, void* arg);

        /// \brief Transfers execution into this context. Returns when the
        /// context calls `switch_out()`. May be called from any thread.
        void switch_in() noexcept;

        /// \brief Transfers execution back to whoever called `switch_in()`.
        /// Must be called from inside this context.
        void switch_out() noexcept;

        /// \brief Marks the context as finished. Called by the entry trampoline
        /// right before its final `switch_out()` so that sanitizer bookkeeping
        /// for this stack can be released.
        void set_finished() noexcept { this->finished_ = true; }

        /// \brief Must be the first thing the entry function does: completes
        /// sanitizer bookkeeping for the initial switch onto this stack.
        void on_first_entry() noexcept;

        [[nodiscard]] bool created() const noexcept { return this->stack_base_ != nullptr; }

        /// \brief Highest address of the usable stack region.
        [[nodiscard]] const void* stack_top() const noexcept { return this->stack_top_; }

        /// \brief Usable stack bytes (guard page excluded).
        [[nodiscard]] std::size_t stack_size() const noexcept { return this->stack_size_; }

    private:
        void* stack_base_{nullptr}; // mapping start (POSIX) / fiber handle (Windows)
        void* stack_top_{nullptr};
        std::size_t stack_size_{0};
        std::size_t mapping_size_{0};
        entry_fn_t entry_{nullptr};
        void* arg_{nullptr};
        bool finished_{false};

#if !defined(_WIN32)
        void* sp_{nullptr}; // saved stack pointer of this context while suspended
        void* host_sp_{nullptr}; // saved stack pointer of the caller while the context runs
        // sanitizer bookkeeping (unused in plain builds)
        void* san_fiber_{nullptr}; // TSan fiber object of this context
        void* san_host_fiber_{nullptr}; // TSan fiber object of the caller
        void* san_host_fake_stack_{nullptr}; // ASan fake stack of the caller while we run
        void* san_fiber_fake_stack_{nullptr}; // ASan fake stack of this context while suspended
        const void* san_host_bottom_{nullptr};
        std::size_t san_host_size_{0};
#else
        void* host_fiber_{nullptr};
        void* thread_fiber_{nullptr};

        static void __stdcall fiber_proc(void* p) noexcept;
#endif
    };
} // namespace usub::uvent::fiber::detail

#endif // UVENT_FIBER_CONTEXT_H
