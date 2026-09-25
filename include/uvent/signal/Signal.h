#ifndef UVENT_SIGNAL_SIGNAL_H
#define UVENT_SIGNAL_SIGNAL_H

#include <atomic>
#include <csignal>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "uvent/sync/AsyncEvent.h"
#include "uvent/tasks/Awaitable.h"

namespace usub
{
    class Uvent;
}

/**
 * \file Signal.h
 * \brief Process signals as awaitables — the tokio::signal model.
 *
 * One process-wide handler per signal number does the only thing that is async-signal-safe: it writes the
 * signal number into a self-pipe. The read end of that pipe is a poll::EventSource on worker 0; when it
 * becomes readable the worker drains it and marks every SignalSet subscribed to that number, waking the
 * coroutines parked in recv(). No signal mask, no dedicated thread, no restrictions on other threads or
 * libraries in the process.
 *
 * Semantics (same as tokio):
 *  - a SignalSet sees every delivery that happens after it was constructed; several deliveries of the
 *    same number before recv() is called collapse into one;
 *  - a delivery before any SignalSet for that number exists is lost (the default disposition still ran);
 *  - installing the handler replaces the default disposition for the lifetime of the process: after the
 *    first `Signal s(SIGINT)`, Ctrl-C no longer kills the program, even when `s` is gone. Call
 *    signal::reset_to_default() if that is not what you want.
 *  - recv() is a normal awaitable: it can be co_awaited, fiber::await'ed, put into select(), and it returns
 *    early (false / -1) when the task is cancelled or the runtime drains.
 *
 * Requirements: construct SignalSet / Signal after the Uvent object exists (the pipe is registered with
 * worker 0 through its inbox; that works before run() as well). SIGKILL and SIGSTOP cannot be caught and
 * throw std::invalid_argument.
 *
 * Windows: SetConsoleCtrlHandler is the source. CTRL_C_EVENT is reported as SIGINT, CTRL_BREAK_EVENT as
 * SIGBREAK, CTRL_CLOSE / LOGOFF / SHUTDOWN as SIGTERM. Other numbers are accepted but never fire. The
 * process is terminated shortly after a CTRL_CLOSE handler returns, so a stop_on() reaction must be quick.
 */
namespace usub::uvent::signal
{
    /**
     * \brief Receiver for a set of signal numbers.
     *
     * `co_await set.recv()` returns the number of the next pending signal in the set, or -1 when the
     * awaiting task was cancelled. Pending numbers are delivered lowest-first; each number is reported once
     * per batch of deliveries.
     */
    class SignalSet
    {
    public:
        explicit SignalSet(std::initializer_list<int> signals);
        explicit SignalSet(std::vector<int> signals);
        ~SignalSet();

        SignalSet(const SignalSet&) = delete;
        SignalSet& operator=(const SignalSet&) = delete;
        SignalSet(SignalSet&&) = delete;
        SignalSet& operator=(SignalSet&&) = delete;

        /// Next pending signal number, or -1 if cancelled.
        task::Awaitable<int> recv();

        /// Consume a pending number without waiting; -1 if none.
        int try_recv() noexcept;

        [[nodiscard]] const std::vector<int>& signals() const noexcept { return this->signals_; }

        /// Called by the driver on worker 0 (internal).
        void deliver(int signo) noexcept;

    private:
        std::vector<int> signals_;
        std::atomic<uint64_t> pending_lo_{0}; // bit i = signal i pending, i < 64
        std::atomic<uint64_t> pending_hi_{0}; // signals 64..127 (real-time range)
        sync::AsyncEvent ev_{sync::Reset::Auto};
    };

    /// \brief Receiver for one signal number (tokio::signal::unix::Signal).
    class Signal : private SignalSet
    {
    public:
        explicit Signal(int signo) : SignalSet({signo}), signo_(signo) {}

        /// True when the signal arrived, false when the task was cancelled.
        task::Awaitable<bool> recv()
        {
            const int s = co_await SignalSet::recv();
            co_return s >= 0;
        }

        bool try_recv() noexcept { return SignalSet::try_recv() >= 0; }

        [[nodiscard]] int signo() const noexcept { return this->signo_; }

    private:
        int signo_;
    };

    /// \brief One-shot: completes on the next SIGINT / Ctrl-C. false if cancelled first.
    task::Awaitable<bool> ctrl_c();

    /// \brief One-shot: completes on the next delivery of any of `signals`, returns which one (-1 if cancelled).
    task::Awaitable<int> wait_any(std::initializer_list<int> signals);

    /**
     * \brief Stop the runtime on a signal, the k8s way: the first delivery calls `rt.stop()` (a graceful
     *        drain when UVENT_RUNTIME_DRAIN is on), a second delivery calls it again, which stops the
     *        workers immediately. Spawns a small task on worker 0; the set lives as long as the runtime.
     */
    void stop_on(usub::Uvent& rt, std::initializer_list<int> signals = {SIGINT, SIGTERM});

    /// \brief SIG_IGN for `signo` (SIGPIPE is already ignored by the library at load time).
    void ignore(int signo);

    /// \brief Restore SIG_DFL for `signo` and forget every subscription to it.
    void reset_to_default(int signo);

    namespace detail
    {
        /// Handler-side entry (also used by the Windows console handler): queue `signo` for delivery.
        void raise_from_handler(int signo) noexcept;
    } // namespace detail
} // namespace usub::uvent::signal

#endif // UVENT_SIGNAL_SIGNAL_H
