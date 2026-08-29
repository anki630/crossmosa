// Host-side unit test for MemoryArena (pure logic, no HAL).
// Pattern follows fat_time_test.cpp / hid_report_map_test.cpp: plain main(),
// gFailures counter.
//
// Why this test carries weight: the arena's whole job is to stop the font
// cache from gambling on heap fragmentation every page turn. Its two failure
// modes are both silent on-device — handing out a block that overlaps a live
// one (corrupt glyphs), and releasing the backing block while pointers into it
// are still held (heap corruption on the holder's later delete[]). Neither
// shows up as an error return, so they have to be pinned here.
#include <MemoryArena.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
int gFailures = 0;

void expectEq(const char* what, long long got, long long want) {
  if (got == want) {
    std::printf("PASS (%s): %lld\n", what, got);
    return;
  }
  std::printf("FAIL (%s): got %lld, want %lld\n", what, got, want);
  gFailures++;
}

void expectTrue(const char* what, bool v) { expectEq(what, v ? 1 : 0, 1); }

// Measured on-device worst case (diag89.log, 2026-08-05):
//   bitmap_cum peak  = 38,076  (all styles on one page)
//   peak_single      = 44,005  (one style's single attempt)
// The shipped arena is 40,960; these constants keep the test honest about
// which of those it actually covers.
constexpr size_t kArenaBytes = 40960;
constexpr size_t kObservedPageBitmaps = 38076;

}  // namespace

int main() {
  {  // Reserve / active / capacity, and idempotence.
    MemoryArena a;
    expectTrue("fresh arena inactive", !a.active());
    expectEq("fresh capacity", static_cast<long long>(a.capacity()), 0);
    expectTrue("reserve succeeds", a.reserve(1024));
    expectTrue("active after reserve", a.active());
    expectEq("capacity after reserve", static_cast<long long>(a.capacity()), 1024);
    // Idempotent: ensureLoaded() calls reserve() unconditionally, and a second
    // call must not throw away a live block (that would strand every pointer
    // already handed out).
    expectTrue("reserve is idempotent", a.reserve(4096));
    expectEq("capacity unchanged by 2nd reserve", static_cast<long long>(a.capacity()), 1024);
    expectTrue("zero-byte reserve fails", !MemoryArena{}.reserve(0));
  }

  {  // Bump allocation: distinct, non-overlapping, in order.
    MemoryArena a;
    a.reserve(1024);
    auto* p1 = static_cast<uint8_t*>(a.alloc(100));
    auto* p2 = static_cast<uint8_t*>(a.alloc(100));
    auto* p3 = static_cast<uint8_t*>(a.alloc(100));
    expectTrue("alloc 1 non-null", p1 != nullptr);
    expectTrue("alloc 2 non-null", p2 != nullptr);
    expectTrue("alloc 3 non-null", p3 != nullptr);
    expectTrue("blocks do not overlap 1-2", p2 >= p1 + 100);
    expectTrue("blocks do not overlap 2-3", p3 >= p2 + 100);
    expectTrue("all owned", a.owns(p1) && a.owns(p2) && a.owns(p3));
    expectEq("used tracks allocations", static_cast<long long>(a.used()), 300);

    // Writing the full extent of each block must not disturb its neighbours —
    // this is the "overlapping block" failure mode, and it is exactly what a
    // wrong alignment/padding calculation produces.
    std::memset(p1, 0xAA, 100);
    std::memset(p2, 0xBB, 100);
    std::memset(p3, 0xCC, 100);
    int bad = 0;
    for (int i = 0; i < 100; i++) {
      if (p1[i] != 0xAA || p2[i] != 0xBB || p3[i] != 0xCC) bad++;
    }
    expectEq("no cross-block corruption", bad, 0);
  }

  {  // Alignment.
    MemoryArena a;
    a.reserve(1024);
    a.alloc(1);  // deliberately misalign the cursor
    auto* p4 = a.alloc(16, 4);
    auto* p8 = a.alloc(16, 8);
    expectEq("4-byte aligned", reinterpret_cast<uintptr_t>(p4) & 3u, 0);
    expectEq("8-byte aligned", reinterpret_cast<uintptr_t>(p8) & 7u, 0);
    // Non-power-of-two and zero alignments are caller errors; the arena must
    // decline rather than compute garbage padding.
    expectTrue("align 0 rejected", a.alloc(4, 0) == nullptr);
    expectTrue("align 3 rejected", a.alloc(4, 3) == nullptr);
  }

  {  // Exhaustion: nullptr (never a partial block), miss counted, arena intact.
    MemoryArena a;
    a.reserve(256);
    expectTrue("fits exactly", a.alloc(256) != nullptr);
    expectEq("no misses yet", static_cast<long long>(a.missCount()), 0);
    expectTrue("one byte over fails", a.alloc(1) == nullptr);
    expectEq("miss counted", static_cast<long long>(a.missCount()), 1);
    expectEq("used unchanged by failed alloc", static_cast<long long>(a.used()), 256);

    // Overflow safety: a huge request must not wrap the capacity arithmetic
    // into "fits". SIZE_MAX here stands in for a corrupted glyph length.
    MemoryArena b;
    b.reserve(256);
    expectTrue("SIZE_MAX request fails", b.alloc(SIZE_MAX) == nullptr);
    expectTrue("near-SIZE_MAX request fails", b.alloc(SIZE_MAX - 8, 8) == nullptr);
    expectEq("used still zero after overflow attempts", static_cast<long long>(b.used()), 0);
  }

  {  // reset() reuses; highWater survives (it is the calibration signal).
    MemoryArena a;
    a.reserve(1024);
    a.alloc(800);
    expectEq("highWater after first page", static_cast<long long>(a.highWater()), 800);
    a.reset();
    expectEq("used zero after reset", static_cast<long long>(a.used()), 0);
    auto* p = a.alloc(800);
    expectTrue("same space reusable after reset", p != nullptr);
    expectEq("highWater not reset by reset()", static_cast<long long>(a.highWater()), 800);
    a.reset();
    a.alloc(900);
    expectEq("highWater rises to new peak", static_cast<long long>(a.highWater()), 900);
  }

  {  // release() guard — the heap-corruption failure mode.
    MemoryArena a;
    a.reserve(1024);
    auto* live = a.alloc(100);
    expectTrue("alloc live", live != nullptr);
    // A holder still points into the block. Releasing here would leave that
    // pointer dangling, and SdCardFont would later delete[] it.
    expectTrue("release refused while used", !a.release());
    expectTrue("still active after refused release", a.active());
    expectTrue("owns() still true after refused release", a.owns(live));
    // Correct order: every consumer drops its pointers, then reset, then release.
    a.reset();
    expectTrue("release succeeds after reset", a.release());
    expectTrue("inactive after release", !a.active());
    expectTrue("alloc after release returns null", a.alloc(4) == nullptr);
    expectTrue("owns() false after release", !a.owns(live));
    expectTrue("release is idempotent", a.release());
    // And it can be re-taken later (ensureLoaded() after a WiFi session).
    expectTrue("re-reserve after release", a.reserve(2048));
    expectEq("re-reserved capacity", static_cast<long long>(a.capacity()), 2048);
  }

  {  // The shipped shape: one reset per PAGE, several styles allocating within
     // that page. Getting this wrong (reset per style) is the one failure mode
     // that silently corrupts the screen — style 2 would hand back memory that
     // style 1 is still rendering from.
    MemoryArena a;
    a.reserve(kArenaBytes);
    for (int page = 0; page < 3; page++) {
      a.reset();  // once per page, NOT per style
      uint8_t* styleBlocks[2];
      for (int style = 0; style < 2; style++) {
        styleBlocks[style] = static_cast<uint8_t*>(a.alloc(kObservedPageBitmaps / 2, 4));
        if (styleBlocks[style] != nullptr) {
          std::memset(styleBlocks[style], style == 0 ? 0x11 : 0x22, kObservedPageBitmaps / 2);
        }
      }
      expectTrue("page: style 0 allocated", styleBlocks[0] != nullptr);
      expectTrue("page: style 1 allocated", styleBlocks[1] != nullptr);
      // Style 0's data must still be intact after style 1 allocated — proving
      // the second style did not reuse the first style's bytes.
      int bad = 0;
      for (size_t i = 0; i < kObservedPageBitmaps / 2; i++) {
        if (styleBlocks[0][i] != 0x11) bad++;
      }
      expectEq("page: style 0 survives style 1", bad, 0);
    }
    expectEq("no misses at observed workload", static_cast<long long>(a.missCount()), 0);
    expectTrue("observed page fits in shipped arena", a.highWater() <= kArenaBytes);
    std::printf("INFO: shipped arena %zu bytes, observed page high-water %zu (%.1f%% used)\n", kArenaBytes,
                a.highWater(), 100.0 * static_cast<double>(a.highWater()) / static_cast<double>(kArenaBytes));
  }

  {  // Honesty check on sizing: the largest SINGLE-style attempt ever measured
     // (44,005) does NOT fit the shipped arena. That is a deliberate trade
     // (see spec §3.3) — such a page falls back to the existing degradation
     // ladder. If someone later enlarges the arena, this assertion tells them
     // the fallback path just became unreachable and needs re-testing.
    MemoryArena a;
    a.reserve(kArenaBytes);
    expectTrue("44,005 single-style page overflows arena (fallback still reachable)", a.alloc(44005) == nullptr);
  }

  std::printf(gFailures ? "memory_arena_test: %d FAILURE(S)\n" : "memory_arena_test: all passed\n", gFailures);
  return gFailures ? 1 : 0;
}
