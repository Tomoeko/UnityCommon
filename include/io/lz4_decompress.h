// SPDX-License-Identifier: GPL-3.0-only

#ifndef LZ4_DECOMPRESS_H
#define LZ4_DECOMPRESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode one raw independent LZ4 block into caller-owned storage. Both lengths
 * must be nonnegative. source may be NULL only when source_bytes is zero;
 * destination may be NULL only when destination_bytes is zero. Nonempty source
 * and destination ranges must be valid, disjoint, and remain live throughout
 * the call. No external dictionary is available. Source bytes are unchanged.
 *
 * This legacy entry point treats destination_bytes as capacity. It returns the
 * decoded length, which may be smaller, or -1 on error. Empty input succeeds
 * with zero output. A match may terminate the block, and the format's final
 * literal/last-match restrictions are not enforced. These permissive behaviors
 * are preserved for existing callers; success is not canonical admission.
 *
 * Both entry points allocate nothing and retain no pointers. On failure the
 * destination may contain a partial decode; callers must discard it. Neither
 * function authenticates backing storage, enforces an application size cap,
 * nor performs transactional publication. Use a private bounded destination.
 * Each encoded byte is consumed at most once and each decoded byte is emitted
 * at most once, including overlapping matches. Callers may precharge the sum
 * of admitted source_bytes and destination_bytes as a conservative byte-work
 * bound, with checked addition in their own work-counter type. This is a byte
 * traversal bound, not an exact instruction or memory-access count. */
int lz4_decompress_safe(
    const uint8_t* source, uint8_t* destination, int source_bytes, int destination_bytes);

/* Enforce complete input and exactly destination_bytes of decoded output in
 * addition to the shared buffer, ownership and failure contract above. Input
 * must contain a token, including for zero output (for example the byte 0).
 * The final sequence contains only literals. If any match occurred, at least
 * five final literals are required and the last match starts at least twelve
 * decoded bytes before the end. A final token's unused match nibble is ignored.
 * Offsets are nonzero little-endian distances into this block's own history;
 * matches may overlap their destination. Return destination_bytes on success
 * or -1 on error. This validates only the LZ4 block, not a frame, checksum,
 * Unity archive, metadata grammar, or source provenance. */
int lz4_decompress_exact(
    const uint8_t* source, uint8_t* destination, int source_bytes, int destination_bytes);

#ifdef __cplusplus
}
#endif

#endif
