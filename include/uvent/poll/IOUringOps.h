#ifndef UVENT_IOURING_OPS_H
#define UVENT_IOURING_OPS_H

#include <coroutine>
#include <cstdint>
#include <vector>

#include <sys/socket.h>

namespace usub::uvent::net
{
    struct SocketHeader;
}

namespace usub::uvent::core::detail
{
    enum class IoOpKind : uint8_t
    {
        Recv,
        RecvMultishot,
        Send,
        Accept,
        AcceptMultishot,
        SendFile,
        Connect,
        WakeFd,
        EventSource,
        /// Generic one-shot op whose completion just fills res/err and resumes coro (fs read/write/fsync).
        File
    };

    struct IoOpBase
    {
        IoOpKind kind{};
        net::SocketHeader* header{nullptr};
        std::coroutine_handle<> coro{};
        ssize_t res{0};
        int err{0};
        bool completed{false};
        bool timed_out{false};

        virtual ~IoOpBase() = default;
    };

    struct RecvOp : IoOpBase
    {
        uint8_t* buf{nullptr};
        size_t len{0};
    };

    struct SendOp : IoOpBase
    {
        const uint8_t* buf{nullptr};
        size_t len{0};
    };

    struct AcceptOp : IoOpBase
    {
        sockaddr_storage addr{};
        socklen_t addrlen{sizeof(sockaddr_storage)};
    };

    struct MultishotAcceptOp : IoOpBase
    {
        std::vector<int> pending_fds; ///< принятые fd, ещё не выданные коротине
        size_t next_fd{0};            ///< голова очереди pending_fds
        bool armed{false};            ///< multishot SQE живёт в ядре
        bool waiting{false};          ///< коротина суспендирована в awaiter'е
    };

    struct MultishotRecvOp : IoOpBase
    {
        struct Seg
        {
            uint32_t len;
            uint32_t off; ///< потреблённый префикс сегмента
            uint16_t bid;
        };

        static constexpr unsigned kInlineSegs = 16;

        Seg segs[kInlineSegs]{};
        unsigned head{0};
        unsigned count{0};
        std::vector<Seg> spill;

        bool armed{false};
        bool waiting{false};
        bool has_terminal{false};
        int32_t terminal{0}; ///< 0 = EOF, <0 = -errno (включая -ECANCELED от таймаута)

        [[nodiscard]] bool has_data() const noexcept { return count != 0 || !spill.empty(); }

        void push(Seg s)
        {
            if (count < kInlineSegs && spill.empty())
                segs[(head + count++) % kInlineSegs] = s;
            else
                spill.push_back(s);
        }

        [[nodiscard]] Seg* front() noexcept
        {
            if (count)
                return &segs[head];
            if (!spill.empty())
                return &spill.front();
            return nullptr;
        }

        void pop_front() noexcept
        {
            if (count)
            {
                head = (head + 1) % kInlineSegs;
                --count;
            }
            else if (!spill.empty())
                spill.erase(spill.begin());
        }
    };

    struct SendFileOp : IoOpBase
    {
        int in_fd{-1};
        off_t* offset{nullptr};
        size_t count{0};
    };

    struct ConnectOp : IoOpBase
    {
        sockaddr_storage addr{};
        socklen_t addrlen{0};
    };
} // namespace usub::uvent::core::detail

#endif // UVENT_IOURING_OPS_H
