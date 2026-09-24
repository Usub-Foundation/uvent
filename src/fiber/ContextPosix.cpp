//
// Created by kirill on 12/09/26.
//

#if !defined(_WIN32)

#include "uvent/fiber/Context.h"

#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "uvent/system/Settings.h"

// Architecture selection (overridable for syntax checks of the other branch).
#if !defined(UVENT_FIBER_ARCH_X86_64) && !defined(UVENT_FIBER_ARCH_AARCH64)
#if defined(__x86_64__)
#define UVENT_FIBER_ARCH_X86_64 1
#elif defined(__aarch64__)
#define UVENT_FIBER_ARCH_AARCH64 1
#else
#error "uvent fibers: unsupported CPU architecture (x86_64 and aarch64 are supported). Configure with -DUVENT_ENABLE_FIBERS=OFF."
#endif
#endif

// ---------------------------------------------------------------------------
// Sanitizer detection
// ---------------------------------------------------------------------------
#if defined(__SANITIZE_ADDRESS__)
#define UVENT_FIBER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UVENT_FIBER_ASAN 1
#endif
#endif

#if defined(__SANITIZE_THREAD__)
#define UVENT_FIBER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UVENT_FIBER_TSAN 1
#endif
#endif

extern "C"
{
    void uvent_fctx_switch(void** from_sp, void* to_sp);
    void uvent_fctx_trampoline();

#if defined(UVENT_FIBER_ASAN)
    void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, size_t size);
    void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, size_t* size_old);
#endif
#if defined(UVENT_FIBER_TSAN)
    void* __tsan_get_current_fiber(void);
    void* __tsan_create_fiber(unsigned flags);
    void __tsan_destroy_fiber(void* fiber);
    void __tsan_switch_to_fiber(void* fiber, unsigned flags);
#endif
}

namespace usub::uvent::fiber::detail
{
    namespace
    {
        std::size_t page_size() noexcept
        {
            static const std::size_t ps = [] {
                long v = ::sysconf(_SC_PAGESIZE);
                return v > 0 ? static_cast<std::size_t>(v) : std::size_t(4096);
            }();
            return ps;
        }

        std::size_t round_up(std::size_t v, std::size_t a) noexcept { return (v + a - 1) / a * a; }

        struct StackCache
        {
            std::size_t mapping_size{0};
            std::vector<void*> free;

            ~StackCache()
            {
                for (void* p : this->free)
                    ::munmap(p, this->mapping_size);
            }
        };

        StackCache& cache() noexcept
        {
            thread_local StackCache c;
            return c;
        }

        void* map_stack(std::size_t mapping_size)
        {
            int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_STACK)
            flags |= MAP_STACK;
#endif
            void* p = ::mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, flags, -1, 0);
            if (p == MAP_FAILED)
                return nullptr;
            if (::mprotect(p, page_size(), PROT_NONE) != 0)
            {
                ::munmap(p, mapping_size);
                return nullptr;
            }
            return p;
        }
    } // namespace

    void Context::create(std::size_t stack_size, entry_fn_t entry, void* arg)
    {
        const std::size_t ps = page_size();
        const std::size_t usable = round_up(stack_size < 4 * ps ? 4 * ps : stack_size, ps);
        const std::size_t mapping = usable + ps;

        void* base = nullptr;
        auto& c = cache();
        if (c.mapping_size == mapping && !c.free.empty())
        {
            base = c.free.back();
            c.free.pop_back();
        }
        else
        {
            base = map_stack(mapping);
            if (!base)
                throw std::bad_alloc();
        }

        this->stack_base_ = base;
        this->mapping_size_ = mapping;
        this->stack_size_ = usable;
        this->stack_top_ = static_cast<char*>(base) + mapping;
        this->entry_ = entry;
        this->arg_ = arg;
        this->finished_ = false;

        // Build the initial frame that uvent_fctx_switch pops on first entry.
        auto* top = reinterpret_cast<std::uintptr_t*>(this->stack_top_);
#if defined(UVENT_FIBER_ARCH_X86_64)
        // Layout (low → high): fpu word, r15, r14, r13=arg, r12=entry, rbx, rbp, ret=trampoline.
        std::uintptr_t* sp = top - 8;
        sp[0] = std::uintptr_t(0x1F80u) | (std::uintptr_t(0x037Fu) << 32); // mxcsr | x87 cw
        sp[1] = 0; // r15
        sp[2] = 0; // r14
        sp[3] = reinterpret_cast<std::uintptr_t>(arg); // r13
        sp[4] = reinterpret_cast<std::uintptr_t>(entry); // r12
        sp[5] = 0; // rbx
        sp[6] = 0; // rbp
        sp[7] = reinterpret_cast<std::uintptr_t>(&uvent_fctx_trampoline); // return address
        this->sp_ = sp;
#elif defined(UVENT_FIBER_ARCH_AARCH64)
        // 176-byte frame: x19..x28, x29, x30, d8..d15, fpcr.
        std::uintptr_t* sp = top - 22;
        for (int i = 0; i < 22; ++i)
            sp[i] = 0;
        sp[0] = reinterpret_cast<std::uintptr_t>(entry); // x19
        sp[1] = reinterpret_cast<std::uintptr_t>(arg); // x20
        sp[11] = reinterpret_cast<std::uintptr_t>(&uvent_fctx_trampoline); // x30
        this->sp_ = sp;
#endif

#if defined(UVENT_FIBER_TSAN)
        this->san_fiber_ = __tsan_create_fiber(0);
#endif
    }

    Context::~Context()
    {
        if (!this->stack_base_)
            return;
#if defined(UVENT_FIBER_TSAN)
        if (this->san_fiber_)
            __tsan_destroy_fiber(this->san_fiber_);
#endif
        auto& c = cache();
        const std::size_t limit = settings::fiber_stack_cache_per_thread;
        if (limit > 0 && (c.mapping_size == this->mapping_size_ || c.free.empty()) && c.free.size() < limit)
        {
            c.mapping_size = this->mapping_size_;
            c.free.push_back(this->stack_base_);
        }
        else
            ::munmap(this->stack_base_, this->mapping_size_);
        this->stack_base_ = nullptr;
    }

    void Context::switch_in() noexcept
    {
#if defined(UVENT_FIBER_TSAN)
        this->san_host_fiber_ = __tsan_get_current_fiber();
        __tsan_switch_to_fiber(this->san_fiber_, 0);
#endif
#if defined(UVENT_FIBER_ASAN)
        __sanitizer_start_switch_fiber(&this->san_host_fake_stack_, this->stack_base_, this->mapping_size_);
#endif
        uvent_fctx_switch(&this->host_sp_, this->sp_);
#if defined(UVENT_FIBER_ASAN)
        __sanitizer_finish_switch_fiber(this->san_host_fake_stack_, nullptr, nullptr);
#endif
    }

    void Context::switch_out() noexcept
    {
#if defined(UVENT_FIBER_TSAN)
        __tsan_switch_to_fiber(this->san_host_fiber_, 0);
#endif
#if defined(UVENT_FIBER_ASAN)
        // On the final switch pass nullptr so ASan releases this stack's fake frames.
        __sanitizer_start_switch_fiber(this->finished_ ? nullptr : &this->san_fiber_fake_stack_,
                                       this->san_host_bottom_, this->san_host_size_);
#endif
        uvent_fctx_switch(&this->sp_, this->host_sp_);
#if defined(UVENT_FIBER_ASAN)
        __sanitizer_finish_switch_fiber(this->san_fiber_fake_stack_, &this->san_host_bottom_,
                                        &this->san_host_size_);
#endif
    }

    void Context::on_first_entry() noexcept
    {
#if defined(UVENT_FIBER_ASAN)
        __sanitizer_finish_switch_fiber(nullptr, &this->san_host_bottom_, &this->san_host_size_);
#endif
    }
} // namespace usub::uvent::fiber::detail

#endif // !_WIN32
