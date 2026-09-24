//
// Created by kirill on 8/26/24.
//

#ifndef UVENT_DEFINES_H
#define UVENT_DEFINES_H

#include <atomic>
#include <iostream>
#include <variant>

#if defined(__APPLE__) || defined(__MACH__)
#define OS_APPLE 1
#endif

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || defined(__BSD__)
#define OS_BSD 1
#endif

#if defined(_WIN32) || defined(_WIN64)
#define OS_WINDOWS 1
#endif

// ---------------- OS-specific includes / types ----------------

#if defined(__APPLE__) || defined(OS_BSD)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef std::variant<sockaddr_in, sockaddr_in6> client_addr_t;

#elif defined(__linux__)

#include <arpa/inet.h>
#include <atomic>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <unistd.h>

#define OS_LINUX

typedef std::variant<sockaddr_in, sockaddr_in6> client_addr_t;

extern std::mutex cout_mutex;

extern void pin_thread_to_core(int core_id);

inline void set_thread_name(std::string &&name, pthread_t thread) {
  pthread_setname_np(thread, name.c_str());
}

inline std::string get_thread_name(pthread_t thread) {
  char buf[16] = {};
  pthread_getname_np(thread, buf, sizeof(buf));
  return buf;
}

#elif defined(OS_APPLE)

#include <pthread.h>

void set_thread_name(const std::string &name) {
  pthread_setname_np(name.c_str()); // only for current thread
}

std::string get_thread_name() { return "unsupported"; }

#elif defined(OS_WINDOWS)

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h> // ALWAYS before windows.h
#include <io.h>
#include <mswsock.h>
#include <process.h>
#include <windows.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")

typedef std::variant<sockaddr_in, sockaddr_in6> client_addr_t;
typedef int socklen_t;

using ssize_t = long long;

inline void set_thread_name(const std::string &name) {
  using SetThreadDescription_t = HRESULT(WINAPI *)(HANDLE, PCWSTR);
  HMODULE h = GetModuleHandleW(L"Kernel32.dll");
  if (h) {
    auto p = reinterpret_cast<SetThreadDescription_t>(
        GetProcAddress(h, "SetThreadDescription"));
    if (p) {
      int wlen = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
      std::wstring wname;
      wname.resize(wlen);
      MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname.data(), wlen);
      p(GetCurrentThread(), wname.c_str());
    }
  }
}

inline std::string get_thread_name() { return "unsupported"; }

#else

#error "Current OS not supported"

#endif

#if defined(OS_WINDOWS)
using socket_fd_t = SOCKET;
constexpr socket_fd_t INVALID_FD = INVALID_SOCKET;
#else
using socket_fd_t = int;
constexpr socket_fd_t INVALID_FD = -1;
#endif

// -------------------- SIGPIPE compatibility --------------------
#if defined(__unix__) || defined(__APPLE__) || defined(__MACH__) || \
    defined(__linux__) || defined(OS_BSD)
#include <csignal>
#include <cstring>

static inline void uvent_ignore_sigpipe_once() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGPIPE, &sa, nullptr);
}

__attribute__((constructor)) static void uvent_ignore_sigpipe_ctor() {
  uvent_ignore_sigpipe_once();
}

#define UVENT_IGNORE_SIGPIPE() uvent_ignore_sigpipe_once()
#else
#define UVENT_IGNORE_SIGPIPE() ((void)0)
#endif

#if defined(__linux__)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#define UVENT_SEND_NOSIG_FLAGS MSG_NOSIGNAL
static inline void uvent_sock_nosigpipe(int) {}

#elif defined(__APPLE__) || defined(OS_BSD) || defined(__MACH__)
#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0x1022
#endif
#define UVENT_SEND_NOSIG_FLAGS 0
static inline void uvent_sock_nosigpipe(int fd) {
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
}

#elif defined(OS_WINDOWS)
#define UVENT_SEND_NOSIG_FLAGS 0
static inline void uvent_sock_nosigpipe(int) {}
#else
#define UVENT_SEND_NOSIG_FLAGS 0
static inline void uvent_sock_nosigpipe(int) {}
#endif

#include <cstddef>
#include <sys/types.h>

// единая обёртка для send без SIGPIPE
static inline
#if defined(OS_WINDOWS)
int
#else
ssize_t
#endif
uvent_send_nosig(socket_fd_t fd, const void *buf, size_t len, int flags) {
#if defined(OS_WINDOWS)
  return ::send(fd, (const char *)buf, (int)len, flags);
#else
  return ::send(fd, buf, len, flags | UVENT_SEND_NOSIG_FLAGS);
#endif
}
// -------------------------------------------------------------------------------

#if UVENT_DEBUG

#include <string>
#include <system_error>
#ifndef OS_WINDOWS
#include <cxxabi.h>
#include <execinfo.h>
#endif
#include <iostream>

extern std::string get_caller_function_name(int depth = 2);

extern void print_caller_function();

#define debug_error_print(message)                                             \
  message +=                                                                   \
      " (" + std::string(__FILE__) + ":" + std::to_string(__LINE__) + ")";     \
  throw std::system_error(errno, std::generic_category(), message);

#endif

// Force-inline hint
#if defined(_MSC_VER)
#define UVENT_ALWAYS_INLINE __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define UVENT_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define UVENT_ALWAYS_INLINE inline
#endif

// Trap
#if defined(_MSC_VER)
#define TRAP() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
#define TRAP() __builtin_trap()
#else
#include <cstdlib>
#define TRAP() std::abort()
#endif

// __builtin_clz portability
#include <cstdint>
#include <mutex>

#if defined(_MSC_VER)
#include <intrin.h>

// On Windows, 'unsigned long' is 32-bit even on x64.
UVENT_ALWAYS_INLINE int clz32(uint32_t x) {
  unsigned long i;
  return _BitScanReverse(&i, x) ? (31 - static_cast<int>(i)) : 32;
}

#if defined(_M_X64) || defined(_WIN64)
UVENT_ALWAYS_INLINE int clz64(uint64_t x) {
  unsigned long i;
  return _BitScanReverse64(&i, x) ? (63 - static_cast<int>(i)) : 64;
}
#else
// 32-bit target: emulate 64-bit with two scans
UVENT_ALWAYS_INLINE int clz64(uint64_t x) {
  uint32_t hi = static_cast<uint32_t>(x >> 32);
  if (hi) return clz32(hi);
  return 32 + clz32(static_cast<uint32_t>(x));
}
#endif

UVENT_ALWAYS_INLINE int clzl_portable(unsigned long x) {
#if defined(_M_X64) || defined(_WIN64)
  return clz64(static_cast<uint64_t>(x));
#else
  return clz32(static_cast<uint32_t>(x));
#endif
}

#elif defined(__clang__) || defined(__GNUC__)

// GCC/Clang builtins are UB on x==0; guard it.
UVENT_ALWAYS_INLINE int clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }
UVENT_ALWAYS_INLINE int clz64(uint64_t x) {
  return x ? __builtin_clzll(x) : 64;
}

// 'unsigned long' width is platform-dependent:
UVENT_ALWAYS_INLINE int clzl_portable(unsigned long x) {
  if constexpr (sizeof(unsigned long) == 8)
    return x ? __builtin_clzl(x) : 64;
  else
    return x ? __builtin_clzl(x) : 32;
}

#else
UVENT_ALWAYS_INLINE int clz32(uint32_t x) {
  if (x == 0)
    return 32;
  int n = 0;
  for (uint32_t bit = 1u << 31; (x & bit) == 0; bit >>= 1)
    ++n;
  return n;
}
UVENT_ALWAYS_INLINE int clz64(uint64_t x) {
  if (x == 0)
    return 64;
  int n = 0;
  for (uint64_t bit = 1ull << 63; (x & bit) == 0; bit >>= 1)
    ++n;
  return n;
}
UVENT_ALWAYS_INLINE int clzl_portable(unsigned long x) {
  if (sizeof(unsigned long) == 8)
    return clz64(static_cast<uint64_t>(x));
  return clz32(static_cast<uint32_t>(x));
}
#endif

#if defined(_WIN32) || defined(_WIN64)
static void wsa_init_once() {
  static std::once_flag f;
  std::call_once(f, []{
      WSADATA wsa{};
      int rc = WSAStartup(MAKEWORD(2,2), &wsa);
      if (rc != 0) {
          throw std::system_error(rc, std::system_category(), "WSAStartup");
      }
  });
}
#endif

#if defined(_MSC_VER)
#  define UVENT_ALWAYS_INLINE_FN __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#  define UVENT_ALWAYS_INLINE_FN inline __attribute__((always_inline))
#else
#  define UVENT_ALWAYS_INLINE_FN inline
#endif

#endif // UVENT_DEFINES_H