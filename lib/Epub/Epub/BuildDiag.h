#pragma once

#include <cstdint>

// Cross-module breadcrumb for the reader's build-failed screen, so a section-build failure can be
// diagnosed on-device WITHOUT a serial cable. The chapter parser records the last expat parse error
// (string + line) here; the reader's showBuildError() renders it alongside the spine index + heap.
// A parse error names the exact XML issue + line; empty parseError + healthy heap => a non-parse
// build failure to investigate; low heap => genuine OOM.
namespace builddiag {
inline char parseError[40] = "";  // last expat parse-error string ("" when the failure was not a parse error)
inline int parseLine = 0;         // XML line number of that error
inline char note[14] = "";        // reason code for a non-parse build failure: busy/inflate/binopen/getbuf/fileread/lowmem/finlz/commit
// v96: largest free block AS SEEN BY startBuild, captured at its first line. The reader's error
// popup used to print ESP.getMaxAllocHeap() at popup time -- which is AFTER the FrameBufferLoan has
// handed the 52,272-byte framebuffer back, i.e. ~52KB lower than what the build actually had to work
// with. The reported "m=16k" therefore said nothing about whether the build was starved.
inline uint32_t maxAllocAtBuildStart = 0;
}  // namespace builddiag
