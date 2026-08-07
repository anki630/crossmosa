/*
 * LD_PRELOAD shim that makes libsmb2's per-connection CONTEXT allocation fail,
 * so smb2_serve_port_async() returns -ENOMEM on the accept path.
 *
 * Why this exists
 * ---------------
 * SmbServer::acceptOneConnection() treats -EIO and -ENOMEM oppositely: -EIO
 * leaves the connection queued and therefore self-repeats on a level-triggered
 * select(), so it is latched; -ENOMEM has already dequeued the connection, so
 * it cannot repeat and must NEVER be suppressed -- it is this project's #1
 * documented crash class (CLAUDE.md hard limit 2; the reason v61 and v62 were
 * shipped) and the DiagLog line is the only evidence that will ever exist on a
 * device with no serial port.
 *
 * That distinction is three lines of C++ and is exactly the kind of thing a
 * later "simplification" collapses back into one boolean. It needs a test that
 * fails when that happens, and the only way to reach the branch is to make
 * smb2_init_context()'s calloc() fail on demand.
 *
 * WHICH allocation -- and why the size RANGE this used to match was a landmine
 * ---------------------------------------------------------------------------
 * An earlier revision matched `nmemb == 1 && 4096 <= size <= 65536` and its
 * comment claimed that shape identified smb2_init_context(). IT DOES NOT. At
 * least two allocations fall inside that window, and both are
 * `calloc(1, sizeof(...))`:
 *
 *   struct smb2_context   ~7,256 B   init.c:303   once per accepted connection
 *   struct smb2_pdu      ~12,568 B   pdu.c:93     ONCE PER PDU
 *
 * (sizes measured on this x86-64 host; they are host- and layout-dependent,
 * which is the second reason not to hardcode either of them.) The range only
 * avoided misfiring because the traffic during the armed window is bare,
 * protocol-free TCP. The moment anyone extends this fixture to a test that
 * actually speaks SMB, a range match starts eating PDU allocations instead, and
 * the symptom would read as "the injection stopped working" rather than "the
 * shim matched the wrong thing".
 *
 * So the match is EXACT, and the exact value is DISCOVERED AT RUNTIME rather
 * than hardcoded:
 *
 *   1. With SMBFAIL_TRACE set, every in-window `calloc(1, size)` appends its
 *      size to that file. Nothing fails during this phase.
 *   2. The test truncates the trace, opens ONE bare TCP connection, and takes
 *      the FIRST size recorded. That is smb2_init_context()'s allocation by
 *      construction: accept_cb() (libsmb2.c:4455) calls it before anything else
 *      on that connection can allocate.
 *   3. The test writes that number into the arm file. Only then, and only for
 *      exactly that size, does this shim fail -- at most SMBFAIL_MAX times.
 *
 * If the shape ever drifts (the first allocation stops being the context, or
 * its size changes) the injection produces no -ENOMEM at all and the test SKIPS
 * with a stated reason. It cannot silently pass.
 *
 * Environment
 * -----------
 *   SMBFAIL_ARM_FILE   path. While it exists, its contents are read as the
 *                      exact byte size to fail. Absent => nothing ever fails.
 *   SMBFAIL_TRACE      path. If set, in-window sizes are appended here.
 *   SMBFAIL_MAX        how many failures to inject in total (default 0 = none)
 *   SMBFAIL_MIN_SIZE   low  end of the TRACE window (default 1024)
 *   SMBFAIL_MAX_SIZE   high end of the TRACE window (default 65536)
 *
 * The window now only bounds what gets TRACED -- keeping the trace short so its
 * first entry is meaningful. It no longer decides what fails.
 *
 * __libc_calloc, not dlsym(RTLD_NEXT, "calloc"): dlsym itself can allocate,
 * which makes the usual interposition recursive and needs a static bootstrap
 * arena whose pointers are then handed to the real free(). glibc exports
 * __libc_calloc, so none of that is necessary here. glibc-only -- and if this
 * file does not compile or load, smb_smoke_test.py SKIPS the test with a stated
 * reason rather than passing quietly.
 *
 * Only raw syscalls (open/read/write/close) are used from inside calloc: no
 * stdio, nothing that could allocate and recurse.
 *
 * Build (the test does this into a temp dir; nothing is left behind):
 *   cc -shared -fPIC -O1 -o libsmbfail.so smbfail_calloc.c
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

extern void *__libc_calloc(size_t nmemb, size_t size);

/* Reads the arm file's contents as a decimal size. <= 0 means "not armed". */
static long armed_size(const char *path) {
  char buf[32];
  ssize_t got;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  got = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (got <= 0) return -1;
  buf[got] = '\0';
  return atol(buf);
}

/* Appends "<size>\n". Hand-rolled decimal conversion so no stdio is involved. */
static void trace_size(const char *path, size_t size) {
  char digits[24];
  char out[32];
  int ndigits = 0;
  int len = 0;
  size_t value = size;
  int fd;

  do {
    digits[ndigits++] = (char)('0' + (value % 10));
    value /= 10;
  } while (value != 0 && ndigits < (int)sizeof(digits));
  while (ndigits > 0) out[len++] = digits[--ndigits];
  out[len++] = '\n';

  fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return;
  if (write(fd, out, (size_t)len) < 0) { /* nothing useful to do about it here */
  }
  close(fd);
}

void *calloc(size_t nmemb, size_t size) {
  static int initialized;
  /* v73: 4096 -> 1024. Patch 4 to the vendored tree (SMB2_MAX_VECTORS 256 -> 32)
   * cut sizeof(struct smb2_context) from 7,256 to 1,880 bytes, which dropped it
   * clean out of the old window -- the trace phase then recorded nothing, the
   * arm phase had no size to arm, and the whole check SKIPPED while looking like
   * it had merely lost LD_PRELOAD. The file's own header warns that a size
   * window is fragile; this is that warning coming true, so the floor now sits
   * well below any plausible context size rather than just below the current
   * one. */
  static size_t min_size = 1024;
  static size_t max_size = 65536;
  static long remaining;
  static const char *arm_file;
  static const char *trace_file;

  if (!initialized) {
    /* getenv() only scans environ; it does not allocate, so this is safe to do
     * from inside calloc during libc startup. */
    const char *s;
    initialized = 1;
    s = getenv("SMBFAIL_MIN_SIZE");
    if (s) min_size = (size_t)atol(s);
    s = getenv("SMBFAIL_MAX_SIZE");
    if (s) max_size = (size_t)atol(s);
    s = getenv("SMBFAIL_MAX");
    remaining = s ? atol(s) : 0;
    arm_file = getenv("SMBFAIL_ARM_FILE");
    trace_file = getenv("SMBFAIL_TRACE");
  }

  if (nmemb == 1 && size >= min_size && size <= max_size) {
    if (trace_file != NULL) trace_size(trace_file, size);
    if (remaining > 0 && arm_file != NULL) {
      long want = armed_size(arm_file);
      if (want > 0 && (size_t)want == size) {
        remaining--;
        return NULL;
      }
    }
  }
  return __libc_calloc(nmemb, size);
}
