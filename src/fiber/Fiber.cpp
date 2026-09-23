//
// Created by kirill on 12/09/26.
//

#include "uvent/fiber/Fiber.h"

#include <cstdlib>

namespace usub::uvent::fiber::detail
{
    namespace
    {
        thread_local FiberBase* t_current = nullptr;
    }

    FiberBase* FiberBase::current() noexcept { return t_current; }

    void FiberBase::start(std::size_t stack_size) { this->ctx_.create(stack_size, &FiberBase::entry, this); }

    void FiberBase::switch_in() noexcept
    {
        this->saved_current_ = t_current;
        this->saved_stack_base_ = system::stack_guard::t_stack_base;
        t_current = this;
        system::stack_guard::set_stack_base(this->ctx_.stack_top());
        this->ctx_.switch_in();
        // Back on the host stack (same thread as before the switch).
        system::stack_guard::t_stack_base = this->saved_stack_base_;
        t_current = this->saved_current_;
    }

    void FiberBase::switch_out() noexcept { this->ctx_.switch_out(); }

    void FiberBase::entry(void* p) noexcept
    {
        auto* self = static_cast<FiberBase*>(p);
        self->ctx_.on_first_entry();
        self->run_body();
        self->finished_ = true;
        self->ctx_.set_finished();
        self->ctx_.switch_out();
        std::abort(); // a finished context is never resumed
    }

    void FiberBase::unwind_if_suspended() noexcept
    {
        if (!this->ctx_.created() || this->finished_)
            return;
        this->unwind_ = true;
        while (!this->finished_)
            this->switch_in();
    }
} // namespace usub::uvent::fiber::detail
