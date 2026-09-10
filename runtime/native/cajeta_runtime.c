// Cajeta language runtime — compiled to LLVM bitcode at compiler build time,
// embedded into the compiler binary, and linker-merged into every user module.
// Keep these helpers small and pointer-only at their ABI boundary.

// macOS feature-test macros, set BEFORE any system header: _XOPEN_SOURCE 600
// re-exposes the ucontext routines the fiber path needs (deprecated in 10.6),
// and _DARWIN_C_SOURCE puts back the BSD entries strict POSIX would hide.
#if defined(__APPLE__)
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 600
#  endif
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE 1
#  endif
#endif

#include <math.h>      // floorf — CPU texture-sample bilinear/nearest filtering
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>   // write(2) for abort-survivable diagnostics (see below)
#include <errno.h>    // ETIMEDOUT — bounded stop-coordinator convergence wait

#include "cajeta_xpu_abi.h"   // single source of truth for the XPU FFI contract

// MinGW-w64 ships no execinfo.h, so backtrace is stubbed on Windows: binaries
// there report empty stack traces, which callers already null-check.
#if defined(_WIN32)
static int backtrace(void** buf, int max) { (void) buf; (void) max; return 0; }
static char** backtrace_symbols(void* const* buf, int n) { (void) buf; (void) n; return NULL; }

// MinGW's _commit(fd) is POSIX fsync(fd) under another name.
#include <io.h>
#include <sys/stat.h>
#define fsync(fd) _commit(fd)

// MinGW's mkdir takes no mode argument, so the helper drops it on Windows.
#define cajeta_mkdir(path, mode) mkdir(path)

// MinGW has neither lstat nor S_ISLNK: stat dereferences instead of inspecting
// and S_ISLNK is 0, so symlink-aware paths see a symlink as its target here.
#define lstat(path, statbuf) stat(path, statbuf)
#define S_ISLNK(mode) 0

// MinGW's _fullpath(NULL, ...) allocates like POSIX realpath; the caller frees.
#include <stdlib.h>
#define realpath(in, _ignored) _fullpath(NULL, (in), 0)
#else
#include <execinfo.h>
#include <sys/utsname.h>   // uname() for the host-triple system property
#define cajeta_mkdir(path, mode) mkdir(path, mode)
#endif

// A portable `malloc_usable_size`, used only by the opt-in poison-on-free path;
// where a platform offers none the shim returns 0 and poisoning skips.
#if defined(__APPLE__)
#  include <malloc/malloc.h>
#  define cajeta_malloc_usable_size(p) malloc_size(p)
#elif defined(_WIN32)
#  include <malloc.h>
#  define cajeta_malloc_usable_size(p) _msize(p)
#elif defined(__GLIBC__) || defined(__linux__)
#  include <malloc.h>
#  define cajeta_malloc_usable_size(p) malloc_usable_size(p)
#else
#  define cajeta_malloc_usable_size(p) ((size_t) 0)
#endif

typedef void (*cajeta_ctor_fn)(void* self);


// === Runtime subsystems: per-capability fragments, each TEXTUALLY #included so
// === the single-TU runtime -> bitcode -> embed build stays one compilation.
#include "cajeta_rt_core.c"
#include "cajeta_rt_prof_instr.c"   // Unit 10: needs core.c's shadow stack
#include "cajeta_rt_prof_trace.c"
#include "cajeta_rt_prof_clock.c"
#include "cajeta_rt_prof_integrity.c"
#include "cajeta_rt_prof_rocm.c"   // U8: before prof_gpu, which selects it
#include "cajeta_rt_prof_vulkan.c" // U13: same — prof_gpu's selector reads it
#include "cajeta_rt_prof_cupti.c"  // U12: loader/binding state (same pattern)
#include "cajeta_rt_prof_gpu.c"
#include "cajeta_rt_shared.c"
#include "cajeta_rt_utf8.c"
#include "cajeta_rt_string.c"
#include "cajeta_rt_ucd_core.c"
#include "cajeta_rt_ucd.c"
#include "cajeta_rt_concurrent_exec.c"
#include "cajeta_rt_concurrent_sync.c"
#include "cajeta_rt_vtable_reflect.c"
#include "cajeta_rt_inject.c"
#include "cajeta_rt_io.c"
#include "cajeta_rt_system.c"
#include "cajeta_rt_hash.c"
#include "cajeta_rt_lang.c"
#include "cajeta_rt_session.c"
#include "cajeta_rt_process.c"
#include "cajeta_arrow.c"   // nucleo-column Arrow C Data Interface shims
#include "cajeta_xpu.c"   // XPU/GPU module

// --- cajeta.io.net: socket intrinsics (BSD sockets / Winsock) ---------------
// Every fragment below reuses this one's fd-ABI helpers, errno shim and
// CAJETA_NET_* ordinals, so each MUST be included after it, in this order.
#include "cajeta_net_socket.c"

#include "cajeta_net_nonblocking.c"

#include "cajeta_net_sockaddr.c"

#include "cajeta_net_getname.c"

// Also needs cajeta_net_sockaddr.c above (storage-size + narrowing helpers).
#include "cajeta_net_listener.c"

#include "cajeta_net_socket_options.c"

#include "cajeta_net_getaddrinfo.c"

// Also needs the R9.4 reactor block above, whose __cajeta_io_wait it delegates
// to on Linux.
#include "cajeta_net_reactor.c"

// The lifecycle drains the engine's live-registration counter and holds the
// body its init delegates to, so it must follow cajeta_net_reactor.c.
#include "cajeta_net_reactor_lifecycle.c"

// SHA-1 for the WebSocket handshake only — never for security.
#include "cajeta_sha1.c"
