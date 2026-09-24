//
// Created by kirill on 12/09/26.
//

#if defined(_WIN32)

#include "uvent/fiber/Context.h"

#include <cstdlib>
#include <new>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace usub::uvent::fiber::detail
{
    namespace
    {
        void* thread_fiber() noexcept
        {
            thread_local void* self = nullptr;
            if (!self)
            {
                if (::IsThreadAFiber())
                    self = ::GetCurrentFiber();
                else
                    self = ::ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
            }
            return self;
        }
    } // namespace

    void __stdcall Context::fiber_proc(void* p) noexcept
    {
        auto* self = static_cast<Context*>(p);
        auto* tib = reinterpret_cast<NT_TIB*>(::NtCurrentTeb());
        self->stack_top_ = tib->StackBase;
        self->entry_(self->arg_);
        std::abort();
    }

    void Context::create(std::size_t stack_size, entry_fn_t entry, void* arg)
    {
        this->entry_ = entry;
        this->arg_ = arg;
        this->finished_ = false;
        this->stack_size_ = stack_size;
        this->mapping_size_ = stack_size;
        void* f =
            ::CreateFiberEx(0, static_cast<SIZE_T>(stack_size), FIBER_FLAG_FLOAT_SWITCH, &Context::fiber_proc, this);
        if (!f)
            throw std::bad_alloc();
        this->stack_base_ = f;
    }

    Context::~Context()
    {
        if (this->stack_base_)
        {
            ::DeleteFiber(this->stack_base_);
            this->stack_base_ = nullptr;
        }
    }

    void Context::switch_in() noexcept
    {
        this->thread_fiber_ = thread_fiber();
        this->host_fiber_ = ::GetCurrentFiber();
        ::SwitchToFiber(this->stack_base_);
    }

    void Context::switch_out() noexcept { ::SwitchToFiber(this->host_fiber_); }

    void Context::on_first_entry() noexcept {}
} // namespace usub::uvent::fiber::detail

#endif // _WIN32
