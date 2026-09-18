#pragma once

#include <cstddef>
#include <cstdint>

// Forward declaration keeps miniz out of consumer translation units; the
// decompressor state is heap-allocated in the .cpp where the type is complete.
struct tinfl_decompressor_tag;

// Streaming deflate decompressor wrapping miniz's tinfl.
//
// Replaces the uzlib-backed InflateReader on the throughput paths (EPUB zip
// entries, PNG IDAT). tinfl decodes via lookup tables where uzlib walks the
// Huffman tree bit-by-bit -- several times faster on this CPU -- at the cost
// of a larger decompressor state (~11KB, transient for the scope of the
// stream; taken from the lent framebuffer bytes via buildscratch::claim()
// when a FrameBufferLoan is active, heap otherwise). FontDecompressor
// intentionally stays on InflateReader:
// its one-shot flash-resident group decompressions are tiny, and the render
// path should not carry the extra state allocation.
//
// Two modes:
//   init(false) -- one-shot: the destination buffer holds the ENTIRE output,
//                  so back-references resolve inside it and no 32KB window is
//                  allocated. read()/readAtMost() must be driven with
//                  contiguous, forward-only slices of that one buffer
//                  (a single read(dest, totalSize) is the common case).
//   init(true)  -- streaming: allocates a 32KB window; output can go to any
//                  buffer in any-sized chunks across calls.
//
// Input is either a single contiguous buffer (setSource) or pulled on demand
// through a fill callback (setFill): return the number of bytes available and
// point *data at them (valid until the next fill call); return 0 at end of
// input. Call setZlibWrapped() before the first read when the stream has a
// zlib header (e.g. PNG IDAT).
class InflateStream {
 public:
  enum class Status {
    Ok,     // Output buffer full; more decompressed data remains.
    Done,   // Stream ended cleanly. produced may be < maxLen.
    Error,  // Corrupt/truncated stream, or decompression failed.
  };

  using FillFn = size_t (*)(void* ctx, const uint8_t** data);

  InflateStream() = default;
  ~InflateStream();
  InflateStream(const InflateStream&) = delete;
  InflateStream& operator=(const InflateStream&) = delete;

  // Allocate decompressor state (and the ring window when streaming) and reset
  // stream state. Releases any previous allocation and allocates afresh on
  // every call. Returns false on OOM (nothing stays allocated).
  //
  // v251: windowBytes sizes the streaming ring (rounded up to a power of two,
  // clamped to [4096, 32768]). A ring at least as large as the stream's
  // TOTAL output never wraps, so every back-reference (distance <= bytes
  // produced so far) resolves correctly -- callers that know the uncompressed
  // size pass it (ringBytesFor) and a 10KB image needs a 16KB ring instead of
  // 32KB. A stream that produces more than that is a size lie; callers already
  // reject output beyond the declared size.
  bool init(bool streaming, size_t windowBytes = 32768);
  static size_t ringBytesFor(size_t totalOutputBytes);
  size_t windowBytes() const { return windowSize; }

  // Free the decompressor state and window.
  void deinit();

  // Provide the entire compressed input as one contiguous buffer.
  void setSource(const uint8_t* src, size_t len);

  // Provide compressed input on demand. ctx is passed back to fn verbatim.
  void setFill(FillFn fn, void* ctx);

  // Declare the input zlib-wrapped (2-byte header + trailing adler32).
  void setZlibWrapped() { zlibWrapped = true; }

  // Decompress exactly len bytes into dest. Returns false if the stream ends
  // or errors before producing len bytes.
  bool read(uint8_t* dest, size_t len);

  // Decompress up to maxLen bytes into dest; *produced gets the byte count.
  Status readAtMost(uint8_t* dest, size_t maxLen, size_t* produced);

 private:
  tinfl_decompressor_tag* state = nullptr;  // ~11KB: heap, or inside the claimed build scratch
  uint8_t* window = nullptr;                // ring (<= 32KB), streaming mode only
  size_t windowSize = 0;                    // ring length, power of two
  uint8_t* arenaBase = nullptr;             // non-null when state/window live in lent framebuffer bytes
  size_t windowPos = 0;                     // ring write cursor
  // Decompressed-but-undelivered region of the window (tinfl can overshoot the
  // caller's requested length; the overshoot waits here for the next read).
  size_t pendingStart = 0;
  size_t pendingLen = 0;

  const uint8_t* inPtr = nullptr;
  size_t inAvail = 0;
  FillFn fill = nullptr;
  void* fillCtx = nullptr;
  bool inputExhausted = false;
  bool zlibWrapped = false;
  bool finished = false;

  // One-shot mode: tinfl needs the output buffer start for back-references.
  uint8_t* oneShotStart = nullptr;
};
