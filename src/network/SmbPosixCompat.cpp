// Task 8: the three POSIX symbols the vendored libsmb2 calls unconditionally
// and that this target does not provide.
//
// THIS FILE EXISTS BECAUSE TASK 8 IS THE FIRST TIME ANY OF lib/smb2/ WAS
// ACTUALLY LINKED INTO THE FIRMWARE. Tasks 1-7 built and tested it natively
// (test/host/), where glibc supplies all three; nothing in the firmware
// referenced smbGetRequestHandlers(), so --gc-sections discarded the whole
// chain and the device link never had to resolve them. Wiring the server into
// CrossPointWebServerActivity made it real, and the link failed immediately:
//
//   socket.c:888: undefined reference to `readv'
//   socket.c:292: undefined reference to `writev'
//   init.c:308:   undefined reference to `getlogin_r'
//
// Why they are missing, and why the fix is a shim rather than a patch:
//
//   * readv / writev. lwIP HAS them -- lwip_readv() and lwip_writev() are both
//     in liblwip.a -- but ESP-IDF sets LWIP_POSIX_SOCKETS_IO_NAMES = 0
//     (lwipopts.h:974), which is what suppresses lwip/sockets.h's
//     `#define readv lwip_readv` aliases, and ESP-IDF's own VFS layer does not
//     implement the scatter/gather calls itself. So the functions exist; only
//     the POSIX spelling does not. These two are therefore pure forwarding,
//     not a reimplementation: one call each, identical semantics, including
//     the short-read/short-write behaviour libsmb2's state machine depends on.
//     Hand-rolling them as a loop of read()/write() would have been a
//     behaviour change (several syscalls where the caller expects one), which
//     is exactly the sort of quiet divergence this project keeps getting
//     bitten by.
//
//   * getlogin_r. Declared by newlib (sys/unistd.h:114) but not implemented
//     for this target. libsmb2 calls it in smb2_init_context() purely to pick
//     a default *client* username; the server path never uses that value (the
//     username arrives from the client's SESSION_SETUP and goes to
//     authorizeUser()). Any non-zero return makes upstream fall back to
//     "Guest" (init.c:308-309), which is what this returns.
//
// NOT put in lib/smb2/: that tree has exactly three authorised divergences,
// machine-checked by scripts/verify_libsmb2_patch.py, and a fourth needs
// the maintainer's approval. None is needed -- this is an addition to *our* tree that
// leaves the vendored sources byte-identical. (The alternative, macro aliases
// via lib/smb2/library.json's build flags, would also have avoided a
// divergence, but it makes the substitution invisible at the call site and
// cannot express the getlogin_r fallback.)
//
// SCOPE WARNING for whoever adds the next network feature: `readv` and
// `writev` defined here are LWIP SOCKET calls. They work on socket descriptors
// only -- passing a file descriptor from the VFS (an SD card file, say) will
// not do what POSIX says. Nothing else in this firmware calls either function;
// if that changes, this needs to become a dispatching implementation rather
// than a forward.
#include <errno.h>
#include <lwip/sockets.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {

ssize_t readv(int fd, const struct iovec* iov, int iovcnt) { return lwip_readv(fd, iov, iovcnt); }

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) { return lwip_writev(fd, iov, iovcnt); }

int getlogin_r(char* name, size_t namesize) {
  (void)name;
  (void)namesize;
  // Non-zero => upstream uses "Guest". See the header comment.
  return ENOSYS;
}

}  // extern "C"
