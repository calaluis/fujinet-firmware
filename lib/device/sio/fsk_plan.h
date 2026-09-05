#ifndef FSK_PLAN_H
#define FSK_PLAN_H

// fsk_plan.h — pure, host-buildable A8CAS FSK parsing/timing rules.
//
// No filesystem, no RMT/GPIO, no FujiNet globals, no allocation, no logging.
//
// The small rule helpers used by the production RMT ISR callback live here so
// host tests and production share exactly the same decode/parity/timing/split
// logic.
//
// The revised V2 design stores a preloaded FSK payload as a table of fixed-size
// blocks rather than as one large contiguous allocation. The pure block-table
// accessors below expose that logical payload without knowing anything about
// ESP-IDF or the filesystem.
//
// fsk_view_step and fsk_preload_into_blocks are implemented in fsk_plan.cpp and
// remain hardware/filesystem independent.

#include <cstddef>
#include <cstdint>

// Force-inline helpers that may be called from the IRAM RMT callback.
// This module deliberately has no ESP-IDF dependency and therefore does not use
// IRAM_ATTR directly.
#if defined(__GNUC__) || defined(__clang__)
#define FSK_FORCE_INLINE static inline __attribute__((always_inline))
#else
#define FSK_FORCE_INLINE static inline
#endif

// Maximum duration accepted by one RMT level-duration field (15 bits).
static constexpr uint32_t FSK_MAX_PORTION_TICKS = 32767;

// RMT runs at 1 MHz: 1 tick = 1 us.
// One A8CAS FSK unit is 1/10 ms = 100 us = exactly 100 RMT ticks.
static constexpr uint32_t FSK_RMT_TICKS_PER_A8CAS_UNIT = 100;

// Scale one A8CAS duration value to the exact 1 MHz RMT tick grid.
FSK_FORCE_INLINE uint32_t fsk_ticks_for_value(uint16_t value)
{
    return static_cast<uint32_t>(value) *
           FSK_RMT_TICKS_PER_A8CAS_UNIT;
}

// Decode one little-endian uint16 from a contiguous two-byte pair.
// Caller guarantees p[0] and p[1] exist.
FSK_FORCE_INLINE uint16_t fsk_decode_le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

// Logical level follows ORIGINAL A8CAS signal-value index parity:
// even index -> logical 0
// odd  index -> logical 1
FSK_FORCE_INLINE bool fsk_level_for_index(size_t value_index)
{
    return (value_index & 1U) != 0;
}

// Return the next RMT-sized portion of a duration.
// remaining_ticks == 0 produces no portion.
FSK_FORCE_INLINE uint32_t fsk_next_portion(uint32_t remaining_ticks)
{
    return remaining_ticks > FSK_MAX_PORTION_TICKS
               ? FSK_MAX_PORTION_TICKS
               : remaining_ticks;
}

// Number of complete uint16 FSK values present in a byte region.
// Any trailing odd byte is intentionally excluded.
FSK_FORCE_INLINE size_t fsk_value_count(size_t data_len_available)
{
    return data_len_available / 2;
}

// -----------------------------------------------------------------------------
// Segmented payload logical-byte access
// -----------------------------------------------------------------------------
//
// `blocks` points to a contiguous table of block pointers.
// Every normal block has `block_size` bytes. The final block may be only partly
// logically used; bounds are governed by the caller's logical payload length.
//
// These helpers perform no bounds checks themselves because the production ISR
// path must remain tiny and deterministic. Callers guarantee that requested
// logical positions are valid.

// Return logical payload byte k from the segmented block table.
// Preconditions:
//   blocks != nullptr
//   block_size > 0
//   k is inside the caller's logical payload length
FSK_FORCE_INLINE uint8_t fsk_block_byte(const uint8_t *const *blocks,
                                        size_t block_size,
                                        size_t k)
{
    return blocks[k / block_size][k % block_size];
}

// Decode a little-endian FSK value from logical positions k and k+1.
//
// Fetching the two bytes independently is intentional: if k is the last byte of
// one preload block, k+1 may reside in the following block.
//
// Preconditions:
//   blocks != nullptr
//   block_size > 0
//   k and k+1 are inside the caller's logical payload length
FSK_FORCE_INLINE uint16_t fsk_block_le16(const uint8_t *const *blocks,
                                         size_t block_size,
                                         size_t k)
{
    const uint16_t lo =
        static_cast<uint16_t>(fsk_block_byte(blocks, block_size, k));
    const uint16_t hi =
        static_cast<uint16_t>(fsk_block_byte(blocks, block_size, k + 1));

    return lo | (hi << 8);
}

// -----------------------------------------------------------------------------
// Host-testable bounded preload helper
// -----------------------------------------------------------------------------

// Injected reader used by fsk_preload_into_blocks.
//
// The reader attempts to place up to `n` bytes into `dst` and returns the number
// actually delivered.
//
// A positive short return is valid and must be accumulated by the preload loop.
// Returning 0 before the requested payload is complete represents EOF/failure
// for this pure helper.
using fsk_read_fn = size_t (*)(void *ctx, uint8_t *dst, size_t n);

// Fill an already-allocated block table with exactly `want` logical bytes where
// possible.
//
// The helper:
//   - never requests more than `read_max` bytes in one reader call;
//   - never writes beyond a block;
//   - never writes beyond logical byte `want`;
//   - accumulates positive short reads;
//   - stops when `want` bytes have been loaded or reader returns 0.
//
// Returns total bytes actually loaded.
// Success is indicated by return value == want.
//
// This function contains no filesystem dependency: production supplies an
// fnio::fread adapter, while host tests supply deterministic stub readers.
size_t fsk_preload_into_blocks(uint8_t *const *blocks,
                               size_t block_count,
                               size_t block_size,
                               size_t want,
                               size_t read_max,
                               fsk_read_fn reader,
                               void *ctx);

// -----------------------------------------------------------------------------
// Pure host-test cursor over a segmented payload
// -----------------------------------------------------------------------------

struct FskChunkView
{
    const uint8_t *const *blocks; // caller-owned immutable block pointer table
    size_t block_size;            // bytes per allocated payload block

    // Logical clamped payload length. A trailing odd byte may exist but is never
    // read as part of a value because fsk_value_count() floors len / 2.
    size_t data_len_available;

    // Original A8CAS value index. This index advances even for zero-duration
    // values so parity of every following signal value is preserved.
    size_t value_index;

    // Logical byte position of the next uint16 pair.
    // Normally == value_index * 2 after all preceding values are consumed.
    size_t byte_pos;

    // State used while one long value is split into multiple <=32767-tick
    // portions.
    uint32_t remaining_ticks;
    bool remaining_level_high;
};

// Initialize a cursor over a caller-owned segmented payload.
//
// For an empty payload:
//   blocks may be nullptr
//   block_size may be 0
// because fsk_view_step will have no complete values to read.
FSK_FORCE_INLINE FskChunkView fsk_view_init(const uint8_t *const *blocks,
                                            size_t block_size,
                                            size_t data_len_available)
{
    return FskChunkView{
        blocks,
        block_size,
        data_len_available,
        0,
        0,
        0,
        false
    };
}

struct FskStep
{
    bool produced;   // true when this step yields one RMT-sized portion
    bool level_high; // logical level of that portion
    uint32_t ticks;  // 1..32767 when produced == true
    bool done;       // true when no waveform work remains
};

// Advance the pure cursor by one emitted portion.
//
// Behavior:
//   - reads only complete 2-byte FSK values;
//   - accesses bytes through fsk_block_le16();
//   - skips zero-duration values while still consuming their original index;
//   - splits long durations incrementally using O(1) state;
//   - never materializes the waveform;
//   - never performs allocation, I/O, logging, or hardware access.
//
// Defined in fsk_plan.cpp.
// It is intended for host tests and is NOT called from the RMT ISR.
FskStep fsk_view_step(FskChunkView &view);

#undef FSK_FORCE_INLINE

#endif // FSK_PLAN_H
