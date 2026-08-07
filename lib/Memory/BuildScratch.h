#pragma once

#include <cstddef>
#include <cstdint>

// Registry for the framebuffer bytes lent out during a build phase
// (GfxRenderer::FrameBufferLoan). The lender (GfxRenderer) deposits the block
// with lend()/reclaim(); a memory-hungry consumer (e.g. InflateStream's ~43KB
// tinfl state + window) may claim() it instead of allocating from the heap.
//
// Exactly one claimant at a time; claim() returns nullptr when the block is
// absent or already claimed, and consumers must fall back to the heap. The
// underlying storage is the framebuffer allocation itself, which is never
// freed -- so even the pathological case (reclaim() while still claimed, which
// logs an error) reads garbage, never freed memory.
namespace buildscratch {

// Lender side (GfxRenderer only).
void lend(uint8_t* buf, size_t len);
void reclaim();

// Consumer side: exclusive claim of the whole block if it is at least minLen
// bytes; nullptr means "use the heap". Release with the same pointer.
uint8_t* claim(size_t minLen, size_t* lenOut = nullptr);
void release(const uint8_t* p);

// On-device diagnostic breadcrumbs (no serial): set by claim()/InflateStream::init so the reader's
// build-failed screen can show why a chapter's 32KB DEFLATE inflate fell back to (and failed on) the
// heap. lastClaim: 'K'=handed out, 'A'=absent (no loan lent), 'S'=block smaller than needed,
// 'C'=already claimed, '?'=none. lastInflate: 'S'=used scratch, 'H'=heap ok, 'W'=heap 32KB window
// malloc FAILED, 's'=state malloc failed, '?'=none.
inline char lastClaim = '?';
inline char lastInflate = '?';

}  // namespace buildscratch
