#include "uvent/signal/Signal.h"

#include <bit>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

#include "uvent/Uvent.h"
#include "uvent/poll/EventSource.h"
#include "uvent/system/SystemContext.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace usub::uvent::signal
{
    namespace
    {
        constexpr int kMaxSignal = 128;

        struct Slot
        {
            std::vector<SignalSet*> sets;
            bool handler_installed{false};
#ifndef _WIN32
            struct sigaction previous{};
#endif
        };

        /**
         * Process-wide state. `pipe_w` is written from the signal handler, everything else is touched under
         * `mtx` from ordinary threads plus the worker-0 driver.
         */
        struct Registry
        {
            std::mutex mtx;
            Slot slots[kMaxSignal];
            core::EventSource source{};
            bool source_registered{false};
            std::atomic<int> pipe_w{-1};
            int pipe_r{-1};
#ifdef _WIN32
            bool console_handler_installed{false};
#endif
        };

        Registry& registry()
        {
            static Registry r;
            return r;
        }

        // ---- driver (worker 0) ----------------------------------------------------------------------------

        void dispatch(int signo) noexcept
        {
            Registry& r = registry();
            std::lock_guard lk(r.mtx);
            if (signo < 0 || signo >= kMaxSignal)
                return;
            for (SignalSet* s : r.slots[signo].sets)
                s->deliver(signo);
        }

#ifndef _WIN32
        void on_pipe_ready(core::EventSource* src, uint32_t) noexcept
        {
            unsigned char buf[256];
            for (;;)
            {
                const ssize_t n = ::read(src->fd, buf, sizeof buf);
                if (n <= 0)
                    break;
                for (ssize_t i = 0; i < n; ++i)
                    dispatch(buf[i]);
            }
        }

        // The only code that runs in signal context: one write(2) of one byte.
        void posix_handler(int signo)
        {
            const int saved = errno;
            const int fd = registry().pipe_w.load(std::memory_order_acquire); // pairs with the release store in ensure_source_locked
            if (fd >= 0)
            {
                const unsigned char b = static_cast<unsigned char>(signo);
                [[maybe_unused]] const ssize_t w = ::write(fd, &b, 1); // EAGAIN when 64 KiB of signals are queued: fine
            }
            errno = saved;
        }
#else
        void on_console_event(core::EventSource*, uint32_t signo) noexcept { dispatch(static_cast<int>(signo)); }

        BOOL WINAPI console_handler(DWORD type)
        {
            int signo;
            switch (type)
            {
            case CTRL_C_EVENT: signo = SIGINT; break;
            case CTRL_BREAK_EVENT: signo = SIGBREAK; break;
            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT: signo = SIGTERM; break;
            default: return FALSE;
            }
            detail::raise_from_handler(signo);
            return TRUE;
        }
#endif

        task::Awaitable<void> register_source_on_worker0()
        {
            Registry& r = registry();
#ifndef _WIN32
            system::this_thread::detail::pl.addSource(&r.source, core::READ);
#endif
            co_return;
        }

        /// Create the pipe and hand its read end to worker 0. Caller holds r.mtx.
        void ensure_source_locked(Registry& r)
        {
            if (r.source_registered)
                return;
            if (!system::global::detail::tls_registry)
                throw std::logic_error("uvent::signal: construct the Uvent runtime before subscribing to signals");
#ifndef _WIN32
            int p[2];
#ifdef __linux__
            if (::pipe2(p, O_CLOEXEC | O_NONBLOCK) != 0)
                throw std::system_error(errno, std::generic_category(), "pipe2 (signal)");
#else
            if (::pipe(p) != 0)
                throw std::system_error(errno, std::generic_category(), "pipe (signal)");
            for (int fd : p)
            {
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
                ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            }
#endif
            r.pipe_r = p[0];
            r.source.fd = p[0];
            r.source.on_ready = &on_pipe_ready;
            r.pipe_w.store(p[1], std::memory_order_release);
#else
            r.source.on_ready = &on_console_event;
#endif
            // The poller is thread_local: registration has to run on worker 0. Its inbox accepts work
            // before the loop starts, so this is valid right after `Uvent rt(n)`.
            system::co_spawn_static(register_source_on_worker0(), 0);
            r.source_registered = true;
        }

        void install_handler_locked(Registry& r, int signo)
        {
            Slot& slot = r.slots[signo];
            if (slot.handler_installed)
                return;
#ifndef _WIN32
            struct sigaction sa{};
            sa.sa_handler = &posix_handler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_RESTART;
            if (::sigaction(signo, &sa, &slot.previous) != 0)
                throw std::system_error(errno, std::generic_category(), "sigaction(" + std::to_string(signo) + ")");
#else
            if (!r.console_handler_installed)
            {
                ::SetConsoleCtrlHandler(&console_handler, TRUE);
                r.console_handler_installed = true;
            }
#endif
            slot.handler_installed = true;
        }

        void check_signo(int signo)
        {
            if (signo <= 0 || signo >= kMaxSignal)
                throw std::invalid_argument("uvent::signal: signal number out of range");
#ifndef _WIN32
            if (signo == SIGKILL || signo == SIGSTOP)
                throw std::invalid_argument("uvent::signal: SIGKILL / SIGSTOP cannot be caught");
#endif
        }
    } // namespace

    namespace detail
    {
        void raise_from_handler(int signo) noexcept
        {
#ifndef _WIN32
            posix_handler(signo);
#else
            Registry& r = registry();
            if (auto* tls = system::global::detail::tls_registry ? system::global::detail::tls_registry->getStorage(0)
                                                                 : nullptr)
                if (auto* p = tls->poller())
                    p->post(&r.source, static_cast<uint32_t>(signo));
#endif
        }
    } // namespace detail

    // ---- SignalSet -------------------------------------------------------------------------------------------

    SignalSet::SignalSet(std::initializer_list<int> signals) : SignalSet(std::vector<int>(signals)) {}

    SignalSet::SignalSet(std::vector<int> signals) : signals_(std::move(signals))
    {
        for (int s : this->signals_)
            check_signo(s);
        Registry& r = registry();
        std::lock_guard lk(r.mtx);
        ensure_source_locked(r);
        for (int s : this->signals_)
        {
            install_handler_locked(r, s);
            r.slots[s].sets.push_back(this);
        }
    }

    SignalSet::~SignalSet()
    {
        Registry& r = registry();
        std::lock_guard lk(r.mtx);
        for (int s : this->signals_)
        {
            auto& v = r.slots[s].sets;
            for (auto it = v.begin(); it != v.end(); ++it)
                if (*it == this)
                {
                    v.erase(it);
                    break;
                }
        }
    }

    void SignalSet::deliver(int signo) noexcept
    {
        if (signo < 64)
            this->pending_lo_.fetch_or(1ull << signo, std::memory_order_release);
        else
            this->pending_hi_.fetch_or(1ull << (signo - 64), std::memory_order_release);
        this->ev_.set();
    }

    int SignalSet::try_recv() noexcept
    {
        for (;;)
        {
            uint64_t v = this->pending_lo_.load(std::memory_order_acquire);
            if (v == 0)
                break;
            const int bit = std::countr_zero(v);
            if (this->pending_lo_.compare_exchange_weak(v, v & ~(1ull << bit), std::memory_order_acq_rel))
                return bit;
        }
        for (;;)
        {
            uint64_t v = this->pending_hi_.load(std::memory_order_acquire);
            if (v == 0)
                break;
            const int bit = std::countr_zero(v);
            if (this->pending_hi_.compare_exchange_weak(v, v & ~(1ull << bit), std::memory_order_acq_rel))
                return bit + 64;
        }
        return -1;
    }

    task::Awaitable<int> SignalSet::recv()
    {
        for (;;)
        {
            if (const int s = this->try_recv(); s >= 0)
                co_return s;
            // ev_ is auto-reset: a deliver() between try_recv() and wait() leaves it set, so wait() returns
            // at once and the loop picks the bit up. A spurious wake (set with no bit — two sets racing) just
            // loops.
            if (!co_await this->ev_.wait())
                co_return -1;
        }
    }

    // ---- helpers ---------------------------------------------------------------------------------------------

    task::Awaitable<bool> ctrl_c()
    {
        Signal s(SIGINT);
        co_return co_await s.recv();
    }

    task::Awaitable<int> wait_any(std::initializer_list<int> signals)
    {
        SignalSet s(signals);
        co_return co_await s.recv();
    }

    namespace
    {
        task::Awaitable<void> stop_on_body(usub::Uvent* rt, std::vector<int> signals)
        {
            SignalSet set(std::move(signals));
            if (co_await set.recv() < 0)
                co_return; // cancelled: the runtime is going down some other way
            rt->stop(); // graceful (drain) when UVENT_RUNTIME_DRAIN is on, immediate otherwise
            if (co_await set.recv() < 0)
                co_return;
            rt->stop(); // second delivery during the drain: Uvent::stop() now stops the workers at once
        }
    } // namespace

    void stop_on(usub::Uvent& rt, std::initializer_list<int> signals)
    {
        // Not task::spawn'ed on purpose: a drain cancels registered tasks, and this one must survive the
        // drain to be able to escalate on the second signal.
        system::co_spawn_static(stop_on_body(&rt, std::vector<int>(signals)), 0);
    }

    void ignore(int signo)
    {
        check_signo(signo);
#ifndef _WIN32
        struct sigaction sa{};
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        if (::sigaction(signo, &sa, nullptr) != 0)
            throw std::system_error(errno, std::generic_category(), "sigaction(SIG_IGN)");
#endif
        Registry& r = registry();
        std::lock_guard lk(r.mtx);
        r.slots[signo].handler_installed = false;
    }

    void reset_to_default(int signo)
    {
        check_signo(signo);
        Registry& r = registry();
        std::lock_guard lk(r.mtx);
#ifndef _WIN32
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        ::sigaction(signo, &sa, nullptr);
#endif
        r.slots[signo].handler_installed = false;
        r.slots[signo].sets.clear();
    }
} // namespace usub::uvent::signal
