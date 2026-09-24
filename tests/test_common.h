#ifndef UVENT_TEST_COMMON_H
#define UVENT_TEST_COMMON_H

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "CHECK failed: %s at %s:%d\n", #cond, __FILE__, __LINE__);                            \
            std::abort();                                                                                              \
        }                                                                                                              \
    }                                                                                                                  \
    while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _va = (a);                                                                                                \
        auto _vb = (b);                                                                                                \
        if (!(_va == _vb))                                                                                             \
        {                                                                                                              \
            std::fprintf(stderr, "CHECK_EQ failed: %s == %s (%lld vs %lld) at %s:%d\n", #a, #b, (long long)_va,        \
                         (long long)_vb, __FILE__, __LINE__);                                                          \
            std::abort();                                                                                              \
        }                                                                                                              \
    }                                                                                                                  \
    while (0)

struct TestCase
{
    const char* name;
    void (*fn)();
};

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <dirent.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#ifdef UVENT_TASK_INTROSPECTION
#include "uvent/system/Introspection.h" // hang dump also lists live tasks
#endif

// UVENT_TEST_HANG_DUMP=<seconds>: a watchdog thread that, after that many
// seconds, makes every thread of the process print its backtrace (SIGUSR2 +
// backtrace_symbols_fd) and then exits with 70. Works without ptrace, so it is
// the way to see where a test hangs under kernel.yama.ptrace_scope=1. Resolve
// the "+0x..." offsets with: addr2line -e <test binary> -f -C -i 0x....
inline void hang_dump_handler(int)
{
    void* frames[48];
    const int n = ::backtrace(frames, 48);
    char head[64];
    const int len = std::snprintf(head, sizeof(head), "\n--- thread %ld ---\n", static_cast<long>(::syscall(SYS_gettid)));
    ::write(2, head, static_cast<size_t>(len));
    ::backtrace_symbols_fd(frames, n, 2);
}

inline void install_hang_dump()
{
    const char* v = std::getenv("UVENT_TEST_HANG_DUMP");
    if (!v || !*v)
        return;
    const int secs = std::atoi(v);
    if (secs <= 0)
        return;
    struct sigaction sa{};
    sa.sa_handler = &hang_dump_handler;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGUSR2, &sa, nullptr);
    std::thread(
        [secs]
        {
            std::this_thread::sleep_for(std::chrono::seconds(secs));
            std::fprintf(stderr, "\n[hang-dump] %d s elapsed, dumping all threads\n", secs);
#ifdef UVENT_TASK_INTROSPECTION
            usub::uvent::introspection::dump(stderr); // live tasks: name, wait reason, wait time
#endif
            // UVENT_TEST_HANG_CMD: a shell command to run at this point, e.g.
            // "ss -tanpi | grep 2441" to see the TCP queues of the hung sockets.
            if (const char* cmd = std::getenv("UVENT_TEST_HANG_CMD"); cmd && *cmd)
            {
                std::fprintf(stderr, "[hang-dump] $ %s\n", cmd);
                std::fflush(stderr);
                [[maybe_unused]] const int rc = std::system(cmd);
            }
            const pid_t me = ::syscall(SYS_gettid);
            if (DIR* d = ::opendir("/proc/self/task"))
            {
                while (dirent* e = ::readdir(d))
                {
                    const long tid = std::atol(e->d_name);
                    if (tid > 0 && tid != me)
                        ::syscall(SYS_tgkill, ::getpid(), tid, SIGUSR2);
                }
                ::closedir(d);
            }
            std::this_thread::sleep_for(std::chrono::seconds(2)); // let the handlers print
            ::_exit(70);
        })
        .detach();
}
#endif

// Each test case runs in a forked child that leaves through _exit(), which
// skips atexit handlers - including the one clang's -fprofile-instr-generate
// uses to dump coverage counters. UVENT_COVERAGE (a PUBLIC definition of the
// instrumented library) tells us the profile runtime is linked in; a weak
// reference would do on ELF, but Mach-O's ld rejects an undefined weak symbol
// in a plain static link. (Give LLVM_PROFILE_FILE a %p so children do not
// overwrite each other.)
#ifdef UVENT_COVERAGE
extern "C" int __llvm_profile_write_file(void);
#endif

inline void flush_coverage_profile()
{
#ifdef UVENT_COVERAGE
    __llvm_profile_write_file();
#endif
}
#endif

#ifndef _WIN32
// Blocking connect to loopback:port from a plain thread, retried until the
// runtime's listener is up. A fresh socket per attempt: on BSD/macOS a socket
// whose connect() failed cannot be connected again (EINVAL), only Linux allows
// the retry on the same descriptor.
inline int connect_blocking(uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int last_errno = 0;
    for (int i = 0; i < 200; ++i)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fd >= 0);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            return fd;
        last_errno = errno;
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::fprintf(stderr, "connect_blocking: port %u never accepted (errno %d)\n", unsigned(port), last_errno);
    std::abort();
}

// A listener whose accept queue is full: the kernel then drops further SYNs,
// so a connect() to it sits in SYN_SENT until the caller gives up. That is the
// only deterministic "black hole" that needs no network at all (public
// TEST-NET addresses may be proxied or unroutable depending on the host).
// How many connects a backlog of 1 admits differs per OS (Linux: backlog+1,
// BSD/macOS: fewer), so the queue is filled with non-blocking connects until
// one stays pending: that one proves the port black-holes now.
struct TarpitListener
{
    int listener{-1};
    std::vector<int> held;
    bool available{false}; // false: this OS answers a full queue instead of dropping the SYN
    const char* why{"no SYN black hole on this OS"};

    TarpitListener(const char* ip, int port)
    {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(listener >= 0);
        int one = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        CHECK(::inet_pton(AF_INET, ip, &a.sin_addr) == 1);
        CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        CHECK(::listen(listener, 1) == 0);

        for (int i = 0; i < 16 && !available; ++i)
        {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            CHECK(fd >= 0);
            const int fl = ::fcntl(fd, F_GETFL, 0);
            CHECK(fl != -1 && ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
            const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
            CHECK(rc == 0 || errno == EINPROGRESS);
            held.push_back(fd);
            if (rc == 0)
                continue;
            pollfd pfd{fd, POLLOUT, 0};
            const int pr = ::poll(&pfd, 1, 300);
            CHECK(pr >= 0);
            if (pr == 0)
            {
                available = true; // SYN dropped: the queue is full from here on
                break;
            }
            int err = 0;
            socklen_t len = sizeof(err);
            CHECK(::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0);
            if (err != 0)
            {
                // e.g. FreeBSD: the syncache answers the SYN and resets on the
                // final ACK (ECONNRESET) - no black hole to be had here.
                static char buf[96];
                std::snprintf(buf, sizeof(buf), "full accept queue answers with errno %d, not a SYN drop", err);
                why = buf;
                break;
            }
        }
        if (!available)
            std::fprintf(stderr, "TarpitListener: %s\n", why);
    }

    TarpitListener(const TarpitListener&) = delete;
    TarpitListener& operator=(const TarpitListener&) = delete;

    ~TarpitListener()
    {
        for (int fd : held)
            ::close(fd);
        ::close(listener);
    }
};
#endif

inline void run_one_test(const TestCase& t)
{
#ifndef _WIN32
    // UVENT_TEST_NOFORK=1 runs the case in this process: needed to debug a hang
    // under gdb (follow-fork does not combine well with an asynchronous run).
    if (const char* nofork = std::getenv("UVENT_TEST_NOFORK"); nofork && *nofork == '1')
    {
#ifdef __linux__
        install_hang_dump();
#endif
        t.fn();
        return;
    }
    const pid_t pid = fork();
    if (pid == 0)
    {
#ifdef __linux__
        // Die with the harness: a `timeout` that kills the parent must not leave
        // a child with live workers behind. Such orphans keep listening on the
        // test ports and, with SO_REUSEPORT, steal connections from later runs.
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() == 1)
            ::_exit(1);
        install_hang_dump();
#endif
        t.fn();
        flush_coverage_profile();
        _exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        std::fprintf(stderr, "[FAIL] %s (status %d)\n", t.name, status);
        std::abort();
    }
#else
    t.fn();
#endif
}

inline int run_tests(const std::vector<TestCase>& tests)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(__linux__) && !defined(_WIN32)
    // UVENT_TEST_PTRACE_ANY=1 lets any process of the same user attach a
    // debugger (gdb -p) despite kernel.yama.ptrace_scope=1: the way to get
    // stacks out of a hang. Inherited by the forked children.
    if (const char* p = std::getenv("UVENT_TEST_PTRACE_ANY"); p && *p == '1')
        ::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
    const char* only = std::getenv("UVENT_TEST");
    std::size_t ran = 0;
    for (auto& t : tests)
    {
        if (only && std::string(t.name).find(only) == std::string::npos)
            continue;
        ++ran;
        auto t0 = std::chrono::steady_clock::now();
        run_one_test(t);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[ OK ] %s (%lld ms)\n", t.name, (long long)ms);
    }
    std::printf("%zu tests passed\n", ran);
    return 0;
}

inline unsigned hw_threads()
{
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 4;
}

#endif
