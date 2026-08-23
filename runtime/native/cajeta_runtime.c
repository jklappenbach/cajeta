// Cajeta language runtime — compiled to LLVM bitcode at compiler build time,
// embedded into the compiler binary, and linker-merged into every user module.
//
// Keep these helpers small and pointer-only at their ABI boundary; the optimizer
// inlines and specializes them across user code.

// macOS feature-test macros. Set BEFORE any system header is included.
//
//   - _XOPEN_SOURCE = 600 re-exposes the ucontext.h routines (swapcontext
//     / getcontext / makecontext) that Apple deprecated in 10.6. They
//     still work, just emit -Wdeprecated-declarations warnings. The
//     fiber implementation depends on them; no stackful alternative on
//     macOS until the planned stackless rewrite lands.
//
//   - _DARWIN_C_SOURCE re-exposes BSD extensions that _XOPEN_SOURCE
//     would otherwise hide: flock(2) + LOCK_EX / LOCK_NB / LOCK_UN
//     from <sys/file.h>, O_CLOEXEC from <fcntl.h>, etc. Without it,
//     _XOPEN_SOURCE puts the headers into strict-POSIX mode and the
//     BSD-only entries disappear. Defining both gives us both surfaces.
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

// execinfo.h (backtrace + backtrace_symbols) — glibc + macOS only.
// MinGW-w64 doesn't ship it; stub on Windows so the runtime compiles
// there. Stack-trace capture on Windows uses DbgHelp's
// CaptureStackBackTrace + SymFromAddr; that path lands in a later
// release. For v0.1.x, Windows binaries report empty stack traces
// (callers null-check / handle that already).
#if defined(_WIN32)
static int backtrace(void** buf, int max) { (void) buf; (void) max; return 0; }
static char** backtrace_symbols(void* const* buf, int n) { (void) buf; (void) n; return NULL; }

// MinGW provides _commit(fd) in <io.h> as the equivalent of POSIX
// fsync(fd) — sync a file descriptor's data to disk. Macro-substitute
// so the runtime code reads the same on every platform.
#include <io.h>
#include <sys/stat.h>
#define fsync(fd) _commit(fd)

// MinGW's mkdir takes a single path argument (no mode). POSIX mkdir
// takes (path, mode). Provide a uniform call site via a helper macro
// that drops the mode on Windows.
#define cajeta_mkdir(path, mode) mkdir(path)

// lstat / S_ISLNK don't exist on MinGW. Windows handles symlinks via
// reparse points (FILE_ATTRIBUTE_REPARSE_POINT); a faithful port would
// use GetFileAttributesEx and translate. For v0.1.0 we degrade: lstat
// just calls stat (which dereferences symlinks instead of inspecting
// them) and S_ISLNK returns 0 (every path is "not a symlink"). Means
// symlink-aware code paths treat symlinks as their target on Windows.
// Tracked for v0.1.x.
#define lstat(path, statbuf) stat(path, statbuf)
#define S_ISLNK(mode) 0

// realpath -> MinGW provides _fullpath in <stdlib.h>. Passing NULL as
// the buffer allocates one; the caller frees it (same lifetime
// contract as POSIX realpath).
#include <stdlib.h>
#define realpath(in, _ignored) _fullpath(NULL, (in), 0)
#else
#include <execinfo.h>
#include <sys/utsname.h>   // uname() for the host-triple system property
#define cajeta_mkdir(path, mode) mkdir(path, mode)
#endif

// `malloc_usable_size` ships in different headers per platform, and Apple's
// equivalent is renamed to `malloc_size` (in <malloc/malloc.h>). Conditionalize
// the include + provide a portable shim so the rest of the runtime can stay
// platform-agnostic. Used only by the optional poison-on-free path; if a
// platform has neither, the shim returns 0 (which makes __cajeta_poison_buffer
// silently skip — acceptable since poison-on-free is itself opt-in).
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


// ===========================================================================
// Runtime subsystems — split out 2026-06-29 into per-capability fragments,
// each TEXTUALLY #included so the single-TU runtime->bitcode->embed build is
// unchanged (clang -MD tracks every include; no CMake change). GPU stack is
// grouped behind the cajeta_xpu.c module aggregator.
// ===========================================================================
#include "cajeta_rt_core.c"
#include "cajeta_rt_prof_instr.c"   // Unit 10: needs core.c's shadow stack
#include "cajeta_rt_prof_trace.c"
#include "cajeta_rt_prof_clock.c"
#include "cajeta_rt_prof_integrity.c"
#include "cajeta_rt_prof_rocm.c"   // U8: before prof_gpu, which selects it
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

// ---------------------------------------------------------------------------
// cajeta.io.net — NET-1.1 native socket intrinsics (BSD sockets / Winsock).
//
// Kept in its own reviewable source file and #included here so it rides the
// single-TU runtime → bitcode → embed build without a CMake change (the build
// compiles ONLY cajeta_runtime.c to bitcode; sibling .c files must be textually
// included to be embedded + linker-merged into user modules). See the file
// header in cajeta_net_socket.c for the full ABI + errno-shim rationale.
// ---------------------------------------------------------------------------
#include "cajeta_net_socket.c"

// ---------------------------------------------------------------------------
// cajeta.io.net — NET-1.7 non-blocking mode + WouldBlock-as-a-value intrinsics.
// MUST be included AFTER cajeta_net_socket.c (reuses its fd-ABI helpers,
// cajeta_net_raw_errno, and the CAJETA_NET_* ordinal contract).
// ---------------------------------------------------------------------------
#include "cajeta_net_nonblocking.c"

// ---------------------------------------------------------------------------
// cajeta.io.net — NET-1.2 native sockaddr marshalling intrinsics.
// ---------------------------------------------------------------------------
#include "cajeta_net_sockaddr.c"

// ---------------------------------------------------------------------------
// cajeta.io.net — NET-1.3 native getsockname/getpeername intrinsics.
// MUST be included AFTER cajeta_net_socket.c (reuses its fd-ABI typedefs +
// cajeta_net_from_fd helper).
// ---------------------------------------------------------------------------
#include "cajeta_net_getname.c"

// ---------------------------------------------------------------------------
// cajeta.io.net — NET-1.4 native TcpListener support intrinsics.
// MUST be included AFTER cajeta_net_socket.c + cajeta_net_sockaddr.c (reuses
// their fd narrowing helpers, SOL_SOCKET context, and storage-size helper).
// ---------------------------------------------------------------------------
#include "cajeta_net_listener.c"

// ---------------------------------------------------------------------------
// cajeta.io.net — NET-1.6 native typed socket-option surface. MUST be included
// AFTER cajeta_net_socket.c (reuses its fd-ABI helpers).
// ---------------------------------------------------------------------------
#include "cajeta_net_socket_options.c"

// ---------------------------------------------------------------------------
// cajeta.io.net — NET-2.1 native getaddrinfo (name resolution) intrinsics.
// MUST be included AFTER cajeta_net_socket.c (uses its file-static
// cajeta_net_ensure_init() so WSAStartup ran on Windows).
// ---------------------------------------------------------------------------
#include "cajeta_net_getaddrinfo.c"

// ---------------------------------------------------------------------------
// cajeta.io.net.reactor — NET-3.1 reactor engine intrinsics (init/register/
// deregister/await_readable/await_writable + portable select() probe).
// MUST be included AFTER cajeta_net_socket.c (reuses its file-static
// cajeta_net_from_fd / cajeta_net_ensure_init) AND after the R9.4 reactor
// block that defines __cajeta_io_wait (which it delegates to on Linux).
// ---------------------------------------------------------------------------
#include "cajeta_net_reactor.c"

// ---------------------------------------------------------------------------
// cajeta.io.net.reactor — NET-3.2 reactor lifecycle (lazy init / clean shutdown).
// MUST be included AFTER cajeta_net_reactor.c: its shutdown drains NET-3.1's
// live-registration counter (__cajeta_net_reactor_active_reset) and its init is
// the body NET-3.1's __cajeta_net_reactor_init delegates to. The runtime
// teardown path (__cajeta_task_shutdown, far above) forward-declares + calls
// __cajeta_net_reactor_shutdown from here.
// ---------------------------------------------------------------------------
#include "cajeta_net_reactor_lifecycle.c"

// ---------------------------------------------------------------------------
// cajeta.hash — NET-11.2 SHA-1 (FIPS 180-4), WebSocket handshake only.
//
// Kept in its own reviewable source file and #included here so it rides the
// single-TU runtime -> bitcode -> embed build without a CMake change (the build
// compiles ONLY cajeta_runtime.c to bitcode; sibling .c files must be textually
// included to be embedded + linker-merged into user modules). See the file
// header in cajeta_sha1.c for the not-for-security-use rationale.
// ---------------------------------------------------------------------------
#include "cajeta_sha1.c"
