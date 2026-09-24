//
// Created by kirill on 21/08/26.
//

#ifndef UVENT_SYSTEM_STACK_GUARD_H
#define UVENT_SYSTEM_STACK_GUARD_H

#include <cstddef>
#include <cstdint>

#include "uvent/system/Settings.h"

namespace usub::uvent::system::stack_guard
{
    /// \brief Reference point of the current stack, stored as an integer: it is
    /// only ever used for distance arithmetic, never dereferenced, so no pointer
    /// to a (possibly dead) local escapes and no out-of-object pointer
    /// comparison happens.
    inline thread_local std::uintptr_t t_stack_base = 0;

    /// \brief Use an explicit address as the reference point (fiber stacks).
    inline void set_stack_base(const void* p) noexcept { t_stack_base = reinterpret_cast<std::uintptr_t>(p); }

    /// \brief Use the caller's current stack position as the reference point.
    inline void set_stack_base_here() noexcept
    {
        char probe;
        t_stack_base = reinterpret_cast<std::uintptr_t>(&probe);
    }

    inline bool stack_too_deep() noexcept
    {
        char probe;
        const auto here = reinterpret_cast<std::uintptr_t>(&probe);
        if (!t_stack_base) [[unlikely]]
            return false;
        const std::size_t depth = t_stack_base > here ? t_stack_base - here : here - t_stack_base;
        return depth > settings::max_transfer_stack_depth;
    }
} // namespace usub::uvent::system::stack_guard

#endif // UVENT_SYSTEM_STACK_GUARD_H
