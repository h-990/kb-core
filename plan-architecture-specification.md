# KB Core: Engine Specification

*Working draft 0.2 · C core with C-ABI modules · prepared by Claude for Harsh · 21 October 2026*

## 0. Orientation

### 0.1 What this document is

A remake of the V1 specification from the pasted transcript. It merges that draft, the first reviewer's contradiction list (items A to D plus four underspecified subsystems), its eight-question checklist, and every decision you made afterwards.

- **Order:** bottom-up. Primitives first, then storage, index, durability, memory, formats, modules, operations, analysis.
- **Labels:** *Locked* = your decision, kept. *Changed* = I replaced an earlier choice and give the reason. *New* = a gap in the earlier drafts.
- **Verification:** the path code (section 1.4), the header and segment arithmetic (section 2) and the size tables were checked by script. The C header in section 7 compiles clean as C11 and C++17. Nothing is benchmarked, so every speed figure is an estimate to be measured.

### 0.2 Short answers to your questions

1. **Are headers dynamically expandable?** Yes, but not by growing the header. A fixed 64-byte core never moves or resizes. Everything variable (children, extents, thumbnail, hooks) lives in out-of-line blocks that the core points to, and a block grows by copy-bigger-and-swap inside one transaction (section 2.7).
2. **Hole management?** Included. The allocator always takes the lowest-address free space first, which behaves as a priority queue by address. It is built from per-segment bitmaps with summaries, slab partial-run lists and a frontier for when nothing fits. Freed space is reusable only after the freeing transaction is durable (section 2.4).
3. **Did the other spec cover every earlier point?** No. It dropped several points and contradicted others. Section 10 is a point-by-point audit with the fix for each.
4. **Hole punching and truncation:** removed. Nothing goes back to the OS unless you call an explicit command (section 2.4.6).
5. **Speed and footprint:** the layout was redone for both: 64-byte header, relative-only identity, 5-byte references, front-coded index, out-of-place data writes, group commit. Per-object numbers are in section 9.

### 0.3 Decision register

| ID | Decision | Status | Where |
| --- | --- | --- | --- |
| K1 | C11 core, C ABI, modules in any language | Locked | 7 |
| K2 | Modules in-process, full access, own threads and hardware | Locked | 7 |
| K3 | pool.db is truth; index.db and maps are derived and rebuildable; WAL for atomicity | Locked | 2, 3, 4 |
| K4 | Hard RAM cap, index-driven loading, explicit buffer pool (no mmap) | Locked | 5 |
| K5 | Child numbers by creation time; visual order lives in the parent payload | Locked | 1.4, 6 |
| K6 | Frozen paths; frozen nodes cannot move or be deleted | Locked | 2.6, 8 |
| K7 | Native type 0 object is an S-expression document | Locked | 6 |
| K8 | Payload as extents with owner tags on fragments | Locked | 2.9 |
| K9 | Structure unencrypted; payload encryption by module | Locked | 5.5 |
| K10 | Secondary and cyclic graphs are module-owned | Locked | 7 |
| K11 | Minimap and defragmenter deferred | Locked | 11 |
| K12 | Nothing returned to the OS unless the user asks | Locked | 2.4.6 |
| C1 | Path component code: 7+1 continuation bit replaced by a prefix-length varint | **Changed** | 1.4 |
| C2 | Headers store relative identity only (parent plus birth number); absolute path is derived and lives in the index | **Changed** | 2.6, 8 |
| C3 | Header is a fixed 64-byte immovable core plus growable out-of-line blocks | **Changed** | 2.7 |
| C4 | Whole-object stitching replaced by windowed payload pinning | **Changed** | 5, 7 |
| C5 | Cross-links store (address, generation); text refers to links by anchor id only | **Changed** | 2.10, 6 |
| C6 | No hash function anywhere; CRC32C only for integrity | **Changed** | 1.3 |
| N1 | Hole management and deferred reuse | New | 2.4 |
| N2 | Two-phase delete | New | 8 |
| N3 | Reserved system objects under the root | New | 2.11 |
| N4 | Slot generation against stale references | New | 2.6 |
| N5 | WAL ring with patch records and data-first commit | New | 4 |
| N6 | Invariants, verify and repair | New | 4.6 |
| N7 | Portability layer for sync, preallocate, truncate | New | 4.7 |

Each Changed item states how to revert it if you disagree.

### 0.4 Glossary

- **Pool:** the file pool.db. **Segment:** a 2 GiB region with its own metadata. **Page:** 4096 bytes by default, the unit of I/O and logging. **Unit:** 64 bytes, the unit of addressing.
- **uref:** a 40-bit unit index. **kb\_addr\_t:** the same location as a byte offset (uref x 64), used in the ABI.
- **Header core:** the fixed 64-byte record that is an object. **Block:** a variable-size structure the core points to. **Slab run:** a group of pages cut into equal slots.
- **Extent:** a contiguous stretch of payload. **Fragment tag:** the 8-byte owner mark at the start of an extent.
- **path\_uid:** the canonical byte string naming an object by position. **Birth number:** a child's creation-order number under its parent.
- **Truth / derived:** truth cannot be recomputed; derived data can be rebuilt from truth.
- **WAL, LSN, checkpoint:** write-ahead log; log sequence number (a byte position in the log stream); the LSN below which the pool is known durable.
- **Lease / pin:** a counted hold that stops eviction.
- **Hole:** free space below the highest used page. **High-water mark (hwm):** the highest used page plus one. **Frontier:** space above the hwm.
- **ABA problem:** an old reference silently pointing at a new object that reuses the same slot.

## 1. Foundations

### 1.1 Conventions

- Little-endian on disk. Fields are read and written through accessor functions that use memcpy. Never cast a page pointer to a packed struct: that is undefined behaviour in C (alignment and strict aliasing).
- CRC32C (Castagnoli) protects all metadata. It has hardware instructions on x86-64 (SSE4.2) and ARMv8.
- Payload integrity is optional per object (section 2.10). Timestamps are not stored in headers; a monotonic change\_seq is.
- Format parameters (page size, unit size, reference width) are fixed at creation and recorded in the superblock. Changing one means migrating the pool.

### 1.2 Addresses and units

| Name | Size | Notes |
| --- | --- | --- |
| Unit | 64 B | one cache line; smallest allocation and the addressing unit |
| Page | 4096 B (format choice 4096 or 16384) | I/O, checksum and log unit |
| Chunk | 512 pages | allocator summary granule |
| Segment | 2 GiB | has its own metadata |
| uref | 40 bits (5 B) | unit index; maximum pool 64 TiB |
| kb\_addr\_t | u64 | uref x 64; 0 is null |

**Why 64-byte units:** one header is one cache line, so one fetch loads a whole header. The 6 low address bits are free, which makes 5-byte references possible and saves 3 bytes on every pointer compared with u64.

**Edge:** unit 0 is the first superblock page and never holds an object, so 0 is a safe null.

### 1.3 Integer codings, by role

| Where | Code | Why |
| --- | --- | --- |
| Header and table fields | fixed-width little-endian (u16, u24, u32, u40, u48, u64) | direct loads, no decode loop |
| path\_uid components | prefix-length varint (section 1.4) | byte order equals numeric order |
| Lengths inside index pages and WAL records | LEB128 | one byte below 128; no ordering needed |
| Sorted ID lists owned by modules | gap coding with Elias gamma or delta, or Elias-Fano | module business; the core does not use them |
| Hashing | none | the sorted index replaces it (decision C6) |

### 1.4 The path\_uid

#### 1.4.1 Definition

The path\_uid of an object is the byte 0x00 (the root marker) followed by the component codes of the birth numbers along the route from the root down to the object. The root itself is the single byte 0x00. It names an object by position, so it is unique while the object stays where it is.

#### 1.4.2 Component code (prefix-length varint)

The number of leading 1 bits in the first byte gives the byte count. Each class adds a base, so every value has exactly one minimal encoding.

| Bytes | First byte pattern | Value range | Payload bits |
| --- | --- | --- | --- |
| 1 | 0xxxxxxx | 1 to 127 (0 is the root marker only) | 7 |
| 2 | 10xxxxxx then 1 byte | 128 to 16,511 | 14 |
| 3 | 110xxxxx then 2 bytes | 16,512 to 2,113,663 | 21 |
| 4 | 1110xxxx then 3 bytes | 2,113,664 to 270,549,119 | 28 |
| 5 | 11110xxx then 4 bytes | 270,549,120 and up (covers all of u32) | 35 |

Encode: find the class, subtract its base, write the remainder big-endian after the class pattern. Lead bytes 0xF8 to 0xFF are reserved. A decoder must reject non-minimal forms.

#### 1.4.3 Properties

Verified by script on more than 24,000 random paths, on every value from 0 to 39,999, and on all class boundaries.

1. **Prefix-free:** the first byte fixes the length, so no code is a prefix of another.
2. **Order-preserving:** bytewise order equals numeric order, across lengths.
3. **Pre-order:** comparing two paths bytewise orders them as a depth-first walk. An ancestor sorts before its descendants, and siblings sort by birth number.
4. **Ancestry is a byte prefix:** if A is a valid path and its bytes begin B, then parsing B from the left reproduces A, so alignment is automatic.
5. **Subtree is one key range:** every extension of P continues with a lead byte below 0xF8, so all of them are below P with 0xFF appended. Any other key at or above P differs from P at an earlier byte and is larger. So the subtree of P is exactly the range from P up to (but excluding) P with 0xFF appended.
6. **Unique canonical form:** keys compare equal only for equal paths.
7. **Zero padding is unambiguous:** after the root byte, 0x00 is never a valid component.

#### 1.4.4 Operations

| Operation | Method | Cost |
| --- | --- | --- |
| Compare | memcmp | O(length) |
| Is A an ancestor of B | byte-prefix test | O(length of A) |
| Depth | parse components from the left | O(depth) |
| Parent | parse to the last component, cut | O(depth) |
| Child path | append the component code | O(1) |
| LCA | longest common prefix, then round down to a component boundary | O(length) |
| Subtree range | P to P+0xFF (exclusive upper bound) | O(1) to build |

Lengths are at most 1024 bytes, so every operation touches a few cache lines.

#### 1.4.5 Limits and edge cases

- KB\_MAX\_PATH\_BYTES = 1024, so the maximum depth is 1023 (all one-byte components). Deeper creation fails with KB\_E\_DEPTH.
- Birth numbers are u32 and start at 1. Numbers 1 to 15 under the root are reserved for system objects (section 2.11). Running out of births under one parent returns KB\_E\_LIMIT; the remedy is a container child.
- Paths are not stored in headers. They live in the index and are derived from parent pointers elsewhere (section 2.6).
- Paths compare as unsigned bytes. Never compare as signed char or through locale-aware routines.

#### 1.4.6 Why this replaces 7+1 (finding C1)

Your 7+1 choice (7 data bits, continuation flag in the top bit, big-endian) was approved on the grounds that memcmp gives tree order. That holds only between one-byte and two-byte components. A counterexample found by brute force: child 16256 encodes as FF 00 and child 16384 as 81 80 00. Numerically 16256 is smaller, but bytewise FF 00 is larger. So under any parent with more than 16,383 children, index order stops matching birth order. Subtree range scans still work, because they rely only on prefixes, but the claim that sorted uid equals pre-order is false.

The fix keeps what you wanted (byte-aligned, compact, memcmp-comparable, nearly the same lengths) by moving the length into the first byte. Lengths match 7+1 at every size, except that the 2-byte class reaches 16,511 instead of 16,383. The one thing lost is local self-synchronization (spotting a boundary from a single byte), which nothing in this design needs, because paths are parsed forward and are short.

**To revert:** keep 7+1, cap any parent at 16,383 children, and document that sibling order is not numeric above that.

## 2. Storage plane

### 2.1 File set

| File | Role | If lost |
| --- | --- | --- |
| pool.db | Truth: headers, blocks, payload, maps, type dictionary | Everything; this is the database |
| index.db | Derived: sorted path\_uid to header address | Rebuilt by traversal (section 3.5) |
| wal.db | Fixed-size ring of committed but not yet checkpointed changes | Pool is consistent as of the last checkpoint; later commits are lost |
| kb.conf | Plain-text user settings: RAM cap, staging size, growth step, I/O mode, durability | Defaults apply |

The index and the WAL record the pool's uuid. A mismatch means they belong to another pool and are ignored.

### 2.2 Superblock

Two copies, at pool pages 0 and 1. An update writes the copy with the lower sequence number and increments it; open takes the valid copy with the higher sequence number. A torn write therefore always leaves one good copy.

| Field | Meaning |
| --- | --- |
| magic, format\_rev | KBPL, layout revision |
| page\_size, unit\_size, uref\_bits, segment\_size | format parameters (section 1.2) |
| pool\_uuid, created\_unix | identity |
| seq | copy sequence number |
| clean\_shutdown | 0 while open for write, 1 after an orderly close with a checkpoint |
| root\_ref | uref of the root header |
| provisioned\_bytes, segment\_count | current file extent |
| hwm\_unit, object\_count | cached values, recomputable |
| wal\_checkpoint\_lsn | the pool is durable below this LSN |
| slab\_ladder\_rev | which size-class ladder is in force |
| feature\_flags | optional services (for example the backlink index) |
| crc32c | over the first 252 bytes; the page is zero-padded after |

### 2.3 Segments, pages and maps

#### 2.3.1 Segment layout

```
segment start
[ 1 page     segment header: magic, segment_id, flags, seq, crc ]
[ 16 pages   free bitmap: 1 bit per page, 1 = used or unprovisioned ]
[ 128 pages  kind map: 1 byte per page ]
[ 2 pages    chunk summary: 1024 chunks x 8 bytes ]
[ 524,141 pages allocatable ]
```

The 147 metadata pages are 588 KiB per 2 GiB, which is 0.028 percent of the pool.

#### 2.3.2 The three maps

| Map | Content | Used for |
| --- | --- | --- |
| Bitmap | used or free, per page | allocation; bit-scan for runs |
| Kind map | page kind per page: free, unprovisioned, segment meta, header slab, block slab, extent data, block run, child tree | scanning only the pages that matter (rebuild, ledger, verify) |
| Chunk summary | per 512-page chunk: free pages, longest free run, free pages at the start, free pages at the end (u16 each) | first-fit search touching 8 KB per segment instead of 64 KB |

The bitmap and the kind map overlap in what they say about free pages. The bitmap is 8 times denser for run finding, and the kind map classifies pages without reading them. The verifier checks that they agree.

#### 2.3.3 Rules

- Extents never cross a segment boundary, because the next segment starts with metadata. The largest single extent is about 2 GiB minus the 588 KiB of metadata; larger payloads use several extents.
- All map pages are ordinary metadata pages and are logged through the WAL (section 4). They are derived data in principle (the verifier can recompute them from headers) but are persisted for fast open.
- Only the last segment may be partly provisioned. Unprovisioned pages are marked used in the bitmap with kind UNPROVISIONED so they cannot be allocated.

### 2.4 Allocator and hole management

#### 2.4.1 Goals

Keep the used area dense toward the start of the file, reuse freed space before extending, make allocation cheap, stay crash-safe, grow in small steps, and never return space to the OS on its own.

#### 2.4.2 Structures

| Structure | Where | Persistent |
| --- | --- | --- |
| Page allocator over bitmap, kind map and summaries | pool.db | yes |
| Slab allocators (header slots, block and payload slots) | pool.db | yes (inside the pages) |
| Hole index: free runs ordered by address, plus size buckets | RAM | no; rebuilt at open, capped in size, an accelerator only |
| High-water mark and frontier | superblock cache | recomputable |

#### 2.4.3 Page allocation: lowest address, first fit

1. If a placement hint is given (an address near the parent or its last child), look in that chunk and its neighbours first for a run of the requested length. Locality wins only when it costs no extra space.
2. Otherwise scan segments in order and chunk summaries in order. A chunk qualifies if its longest run is at least n, or if the end of this chunk joined to the start of the next gives at least n. Runs longer than a chunk use fully free chunks between the two ends.
3. Inside the first qualifying chunk, bit-scan for the lowest run of n free pages.
4. If nothing qualifies, provision more space at the frontier (section 2.4.6) and take it there.
5. Set the bits and the kind, update the summary, and log the three page patches in the caller's transaction.

A per-size cursor in RAM remembers the first chunk worth trying, so dense early segments are not rescanned on every call. Any free in an earlier chunk resets the cursor.

#### 2.4.4 Slot allocation (slabs)

Small objects are cut from slab runs (section 2.5). Each class keeps a list of partially filled runs ordered by address. An allocation takes the lowest run, finds a free slot with a count-trailing-zeros on the run's free mask, and clears that bit. If no run has space, one is allocated through the page allocator. When a run becomes completely free it goes back to the page allocator (inside the pool, not to the OS).

For headers, the first choice is the run holding the parent's most recent child, when it has a free slot. Siblings created together then sit in adjacent slots, so one 4 KB page read loads up to 63 headers of one family.

#### 2.4.5 Deferred reuse (crash safety)

- Space freed by transaction T becomes allocatable only after T is durable. Until then it is held as pending, so an abort or a crash can never expose overwritten data.
- New data is written only to space that was already free before the transaction began, never to space freed inside it.
- Bits set by an allocation are logged in the same transaction as the header patches that reference the space. Either both are durable or neither is. Data written into allocated but uncommitted space is harmless garbage in free space.

#### 2.4.6 Growth and shrink (explicit shrink only)

- **Growth:** when the frontier is needed, the file is extended by grow\_step\_bytes (default 64 MiB, or more if the request is larger) and preallocated with the platform call (section 4.7), so that running out of disk space is found at grow time and not at write time. New pages are marked free in one small logged transaction. If a crash falls between the file extension and the log commit, the superblock still holds the old size: the extra tail is ignored and reused on the next growth.
- **Logical size versus file size:** the logical size is the hwm. The file may be larger (provisioned but unused). The hwm falls on its own when the topmost pages are freed (found by walking the summaries downward).
- **Shrink:** only when the user invokes kb\_pool\_shrink. It takes the hwm rounded up to grow\_step, marks the pages beyond it unprovisioned in one transaction, then truncates the file. It returns the bytes released. Empty trailing segments are dropped.
- **Never done automatically:** truncating a free tail, punching holes in interior free space, or any call that tells the OS to reclaim. Both can be offered later as explicit user commands.

#### 2.4.7 Fragmentation visibility

kb\_pool\_stats reports file size, hwm, free bytes below the hwm and the largest hole. A hole histogram by size class is available to a future minimap or defragmenter. Neither is needed for correctness.

#### 2.4.8 Why not a literal queue of free spots

A first-in-first-out queue would reuse the oldest hole, not the lowest one, and the used region would stay speckled. Lowest-address-first is a priority queue ordered by address. The bitmap with summaries provides it with no persistent footprint, and the in-RAM hole index only speeds it up.

### 2.5 Slab classes

Small structures (header cores, blocks, small payloads) come from slab runs. A run is one or more pages with a 64-byte run header (free mask, class, slot count, crc32c and, for header slabs, a type summary) followed by equal slots. The free mask is one u64, so a run has at most 63 slots.

- **Ladder:** multiples of 64 B up to 512 B, then four classes per doubling up to 16,384 B (28 classes). Neighbouring classes differ by at most 25 percent, so rounding waste is at most 20 percent.
- **Run size:** the smallest of 1, 2, 4, 8, 16, 32 or 64 pages that keeps the run's own waste at or below 7 percent.

| Class (bytes) | Run pages | Slots | Run waste |
| --- | --- | --- | --- |
| 64 | 1 | 63 | 0.0% |
| 128 | 1 | 31 | 1.6% |
| 256 | 1 | 15 | 4.7% |
| 512 | 2 | 15 | 5.5% |
| 1,024 | 4 | 15 | 5.9% |
| 2,048 | 8 | 15 | 6.1% |
| 4,096 | 16 | 15 | 6.2% |
| 8,192 | 32 | 15 | 6.2% |
| 16,384 | 64 | 15 | 6.2% |

**Rules**

- A payload slot holds the 8-byte fragment tag, so usable bytes are the class size minus 8. Above 16,376 payload bytes, an extent is a page run from the page allocator, rounded up to whole pages (waste under one page).
- Header cores use class 64 in dedicated header-slab runs (63 per page) and are never mixed with payload.
- **Growth:** when an object outgrows its class it moves up the ladder (copy, swap, free). When two consecutive appends are seen, the allocator skips rungs and takes double the needed size, so repeated appends copy each byte about once instead of about four times (the 1/(ratio minus 1) estimate for a 1.25 ratio). Over-allocation is trimmed during maintenance.

### 2.6 Object header core

The header core is the object: 64 bytes, 64-byte aligned, immovable for its whole life in V1. Offsets were verified to total 64.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 1 | state, hdr\_rev | low 4 bits state, high 4 bits layout revision |
| 1 | 1 | type\_version | schema version of the payload type |
| 2 | 2 | type\_id | type dictionary id (0 = native S-expression) |
| 4 | 4 | flags | see below |
| 8 | 4 | crc32c | over the 64 bytes with this field zeroed |
| 12 | 5 | parent\_ref | uref of the parent header (0 for the root) |
| 17 | 5 | children\_ref | meaning depends on CHILD\_REPR |
| 22 | 5 | ext0\_ref | inline extent start, or the extent table block |
| 27 | 3 | ext0\_cap\_units | inline extent capacity in 64-byte units |
| 30 | 5 | aux\_ref | aux block (module summary, thumbnail, hooks) |
| 35 | 3 | aux\_len | aux block bytes |
| 38 | 4 | birth\_num | creation-order number under the parent |
| 42 | 4 | next\_child\_counter | birth number the next child receives |
| 46 | 4 | child\_count | number of children |
| 50 | 4 | change\_seq | bumped on any header or payload change; compare for equality only |
| 54 | 4 | slot\_gen | slot generation, bumped each time the slot is freed |
| 58 | 6 | payload\_len | logical payload bytes (u48, maximum 256 TiB) |

**State values:** 0 FREE (generation kept), 1 LIVE, 2 DETACHED (removed from its parent, awaiting reclaim, subtree intact), 3 FORWARD (reserved for relocation stubs in V1.1).

**Flags:** bit 0 PATH\_FROZEN, bit 1 HAS\_FROZEN\_BELOW, bit 2 OBJ\_IMMUTABLE, bit 3 PAYLOAD\_ENCRYPTED, bit 4 HAS\_AUX, bit 5 HAS\_PAYLOAD\_CRC, bits 6-7 CHILD\_REPR (0 none, 1 inline, 2 array, 3 tree), bits 8-9 EXT\_REPR (0 none, 1 inline, 2 table), bit 10 SYSTEM, bits 11-15 reserved, bits 16-31 module-defined (the kernel preserves them).

**Notes**

- **Relative identity (C2).** The header holds no path and no depth, only parent\_ref and birth\_num. Both stay valid when an ancestor moves, so moving a subtree rewrites one header, not every descendant. The absolute path is derived by walking parent\_ref upward and is cached in RAM; it is also the key in the index. Cost: a cold path lookup by address reads up to depth header pages (siblings share pages, so the walk usually touches few). Gain: moves are O(1) in the truth plane, no per-header path bytes (a depth-20 path stored three times would be 60 bytes or more per object), and no resumable cascade for crash recovery. **To revert:** store an absolute-path block per object; a move then rewrites N blocks and N index keys and needs a resumable cascade with a dirty marker on the parent.
- **Slot generation (N4).** A reference is (address, generation). Freeing a header slot increments slot\_gen and keeps it in the FREE image; the next object in that slot inherits the new value. A stale reference fails with KB\_E\_STALE\_REF instead of reading a different object. The 32-bit counter wraps after 4 billion reuses of one slot.
- **change\_seq** lets caches (GUI, modules) validate a copy with one comparison.
- **crc32c:** the kernel always rewrites all 64 bytes with a fresh checksum, so a header update is one 64-byte WAL patch.
- **Frozen semantics:** PATH\_FROZEN on X means X and every ancestor can neither move nor be deleted, and X's birth\_num cannot change. Freezing walks up from X setting HAS\_FROZEN\_BELOW on every ancestor (the root is always frozen). Unfreezing clears X, then walks up clearing HAS\_FROZEN\_BELOW on ancestors that no longer have a frozen child (checked by scanning child headers; rare). Children may still be added under, or moved out of, a frozen node. A frozen path is a permanent address that is safe to give to the outside world.
- **OBJ\_IMMUTABLE:** payload, aux and child list reject writes (KB\_E\_READONLY). The object can still be unfrozen or deleted unless it is frozen.
- No spare bytes remain. A new header field means a new hdr\_rev, or it goes in the aux block.

### 2.7 Header expandability

**Rule.** The core never moves and never changes size, because its address is referenced by the parent's child table, every child's parent\_ref, fragment tags, hooks and the index. Everything that can grow lives in a block that the core points to.

| What grows | Where it lives | Representations | Growth steps |
| --- | --- | --- | --- |
| Children | children\_ref | none, inline (the single child), array block, B+tree | inline becomes array at the 2nd child; array grows x1.5 up to 4,096 entries; then tree |
| Extents | ext0\_ref | none, inline (one extent), table block | inline becomes table at the 2nd extent; table grows x2 |
| Aux (summary, thumbnail, hooks) | aux\_ref | none, or one aux block | rewritten whole, copy-on-write, sized with 25 percent slack |

**Growth procedure** (one transaction, writer lock held):

1. Allocate the larger block (hint: near the old block).
2. Copy the old content, append the new entry, write the new block header with count, capacity and crc32c.
3. Patch the core: new ref (and representation bits), count, change\_seq, new crc32c.
4. Mark the old block freed (pending until commit).
5. Log all patches and commit.

Readers see the old block or the new one, never a mixture, because the swap is a single 64-byte patch of the core. Geometric growth makes each entry move O(1) times on average.

**Limits:** child\_count is u32. Extents per object have a soft limit of 4,096; beyond it KB\_E\_TOO\_FRAGMENTED asks for a merge. An aux block is at most 16 MiB (aux\_len is u24).

**Shrinking** happens only in maintenance, when utilization falls below one quarter, with hysteresis. Freed space returns to the pool, not to the OS.

**What cannot expand:** the core itself. Anything that does not fit goes into aux, or into a new hdr\_rev; the verifier refuses unknown revisions.

### 2.8 Child tables

**Entry** (10 bytes, packed): birth\_num u32, child\_ref u40, flags u8 (reserved, zero). **Array block:** a 16-byte block header (kind u8, class u8, flags u16, count u32, capacity u32, crc32c u32) followed by entries sorted by birth\_num. Births are monotonic, so appends land at the end.

| Children | Representation | Lookup by birth number | Append | Remove |
| --- | --- | --- | --- | --- |
| 0 | none | n/a | becomes inline | n/a |
| 1 | inline: children\_ref is the child's uref | direct | becomes array | becomes none |
| 2 to 4,096 | array block (at most about 40 KB) | binary search | O(1) amortized | memmove of at most 40 KB (microseconds) |
| more | B+tree keyed by birth\_num (u32 key, 5-byte value), same code as the index | O(log) in pages | O(log) | O(log) |

- **Hysteresis:** tree back to array only below 1,024 entries, array back to inline only when one child remains, both only in maintenance.
- **Order:** creation order only. Visual order lives in the parent payload (K5).
- **Invariants:** each entry's child has parent\_ref equal to this parent and the same birth\_num; child\_count equals the number of entries.

### 2.9 Extents and fragments

**Representations:** none (payload\_len is 0); inline (one extent, with ext0\_ref and ext0\_cap\_units in the core, used bytes equal to payload\_len); table.

**Extent table block:** the 16-byte block header, then 16-byte entries: ref u40, flags u8, cap\_units u32, used u48. Entries are in logical order; logical offsets are prefix sums of used. The inline capacity field is u24 units, so an inline extent holds at most 1 GiB; a larger single extent uses a table.

**Fragment tag:** the first 8 bytes of every extent: owner\_ref u40, extent\_idx u16, tag\_crc8 u8. Payload bytes start at offset 8. Purposes: leak and orphan detection by the verifier, forensic repair, and a fragment deleted on its own can tell its owner to revoke the entry. Cost: 8 bytes per extent, plus a rounding effect for exact sizes (an 8,192-byte payload needs 8,200 bytes and lands in the 10,240 class).

**Write paths**

- **Append in place:** if the last extent has spare capacity, write the new bytes into the spare (this destroys no committed data), sync the pool file, then commit the new used length and payload\_len. A crash before the commit leaves the old length.
- **Replace:** allocate new extents (length plus slack hint), write tag and data, sync the pool, then commit the pointer swap. Old extents are freed after the commit.
- Extent boundaries are invisible to modules: they read windows through the buffer pool (section 5.3).

**Slack hint** is per type (type 0: 25 percent; immutable media: 0). Merging fragments is the deferred defragmenter's job (K11); the structures already support it.

**Integrity:** the tag crc8 always; payload CRC32C optionally, through flag HAS\_PAYLOAD\_CRC (the value is in the aux directory).

### 2.10 Aux block

Layout: a 24-byte directory, then meta bytes, thumbnail bytes and hook records, each part padded to 8.

**Directory:** aux\_rev u8, thumb\_fmt u8, meta\_len u16, thumb\_len u32, hook\_count u32, next\_anchor\_id u32, payload\_crc32c u32, dir\_crc32c u32 (24 bytes).

**Hook record** (24-byte fixed part, then locator bytes padded to 8): anchor\_id u32, graph\_type\_id u16, flags u8, loc\_len u8, target\_gen u32, target\_ref u40, 7 bytes of padding.

- Module meta (codec, duration, canvas bounds and so on) and the thumbnail are produced by the type handler's extract\_summary after each payload commit. They let the viewport draw without running the module (tier 1, section 5.2).
- Hooks are preload hints plus opaque locators. The kernel stores and returns them but does not interpret locators and does not own the graph (K10).
- Text refers to a hook by anchor id only (C5). The target lives in one place, so text and header cannot disagree.
- An update rewrites the whole aux block out of place (it is small), then swaps aux\_ref in the core.
- **Limits:** 16 MiB per aux block; thumbnails should stay under 64 KB; a locator is at most 255 bytes (a canvas rectangle with zoom needs about 36).

### 2.11 System objects, type dictionary, ledger

**Root:** its header is at the superblock's root\_ref; path 0x00; always frozen; flag SYSTEM. Reserved children of the root:

| Birth | Object | Notes |
| --- | --- | --- |
| 1 | Type dictionary | frozen; payload is the list of type records |
| 2 | Settings | optional persisted copies of user limits |
| 3 | Reclaim queue | detached subtree roots awaiting two-phase delete (section 8) |
| 4 | Lost and found | orphans adopted by repair |
| 5 to 15 | reserved |  |

User content starts at birth number 16 (the root's next\_child\_counter starts at 16).

**Type record** (64 bytes): type\_id u16, version u8, flags u8, name 32 bytes, module\_uuid 16 bytes, ref\_map\_ref u40 (optional, for relocation), 7 reserved. Type ids: 0 is the native S-expression; 1 to 255 are core-reserved; 256 and up are assigned to modules and persisted against the module uuid, so ids are stable across runs.

**Unknown types:** a type whose module is not loaded is opaque but fully traversable and movable, because structure belongs to the kernel. The viewport shows its cached meta and thumbnail.

**Ledger:** there is no global per-type table (it would cost 5 or more bytes per object). Each header-slab run header carries a 16-byte type summary (up to 8 distinct type ids plus an overflow flag). ledger\_by\_type reads only the runs whose summary may contain the type, in address order (sequential I/O). An exact per-type list can be built by a module on the ordered-table service (section 7.6).

## 3. Index plane

### 3.1 Contract

index.db maps path\_uid to header uref and nothing else: no thumbnails, no module data. It is derived: it can be deleted and rebuilt. Every hit is validated against the header (state must be LIVE). It is the only structure ordered by path.

### 3.2 File and page format

**Page 0:** magic KBIX, pool\_uuid, root\_page, height, page\_count, free\_list\_head, key\_count, applied\_lsn, crc32c. All other pages are node pages of one page size.

**Node page:** a 32-byte header (kind u8, level u8, nkeys u16, nrestarts u16, flags u16, right\_sibling u32, crc32c u32, 16 reserved).

- Leaf entries are front-coded against the previous key: shared length (LEB128), unshared length (LEB128), unshared bytes, value (header uref, 5 bytes). Every 16th entry is a restart with a full key. Restart offsets (u16 each) sit at the end of the page, so a lookup binary-searches the restarts and then scans at most 16 entries.
- Internal nodes hold separators cut to the shortest distinguishing prefix, plus 4-byte child page numbers. Leaves are chained by right\_sibling for range scans.
- **Split policy:** an insert into the rightmost leaf splits leaving the left page full. Births are monotonic, so appends are the common pattern; this gives near 100 percent fill instead of about 69 percent for random inserts.

### 3.3 Operations

- **Lookup:** descend, binary-search restarts, scan, validate the header.
- **Insert and delete:** O(log) in pages, logged as logical records (section 3.7).
- **Range scan:** seek to the lower bound, walk right siblings until the upper bound.
- **Range move (the payoff of C2):** the subtree of A is the contiguous range from A to A with 0xFF appended. A move extracts that range, replaces the prefix A with the new prefix, and bulk-inserts the result as one contiguous range. Suffix order is unchanged, so the new range is already sorted; pages are built sequentially and spliced in. Cost is O(N/B + log) page operations for N keys.
- **Range delete:** drops the pages of a range wholesale and rewrites the two boundary leaves.

### 3.4 Size and depth (illustration)

Assumption: about 9 bytes per leaf entry (two length bytes, about 2 suffix bytes, a 5-byte value) and 4 KB pages, which gives about 439 entries per leaf.

| Objects | Leaf pages | Leaf bytes | Levels | Internal pages |
| --- | --- | --- | --- | --- |
| 10^7 | 22,761 | 0.09 GB | 3 | about 0.2 MB |
| 10^8 | 227,608 | 0.93 GB | 4 | about 2 MB |
| 10^9 | 2,276,083 | 9.3 GB | 4 | about 21 MB |
| 10^10 | 22,760,827 | 93 GB | 4 | about 213 MB |

Consequence: with about 21 MB of internal pages resident, a lookup at a billion objects costs about one cold page read (the leaf). The earlier claim that ten million objects fit in a few MB was off by about 20 times; the figure is about 90 MB.

### 3.5 Rebuild

1. Create an empty index.db and bind it to the pool uuid.
2. Walk depth-first from the root over child tables, emitting (path, uref) in key order. Siblings usually sit adjacent in header slabs, so reads are close to sequential; prefetch the next children's pages while processing the current one.
3. **Parallelism:** give each top-level subtree to a worker. Each produces a sorted run; runs are concatenated in key order and bulk-loaded bottom-up.
4. **Orphan sweep:** read the kind map to list header-slab pages, scan them for LIVE headers the walk did not reach, adopt each under lost and found (system object 4) and report them.
5. Set applied\_lsn to the pool's current LSN.

Cost is I/O bound by the header slabs: about 64 GB read for a billion objects.

### 3.6 Concurrency

Index mutation happens only inside the commit pipeline (section 4.3), so there is one writer. Readers take shared page latches with latch coupling; the writer takes exclusive latches only while changing a page. Path lookups into a range that is being moved or deleted return KB\_E\_INDEX\_BUSY and are retried (milliseconds).

### 3.7 Durability of the index

The WAL carries logical index records (insert, delete, range move begin and end, range delete begin and end), not page images. index.db is flushed on its own schedule and stamps applied\_lsn. Recovery replays records after applied\_lsn; the operations are idempotent. An unfinished range operation is repaired by re-derivation: delete both ranges, then re-index the moved subtree from the truth by a depth-first walk. If index.db is missing or fails validation, rebuild it (section 3.5).

## 4. Durability plane

### 4.1 Model

Data first, metadata through the log. Payload bytes go out of place into free space and are synced. Everything else (headers, blocks, maps) changes through byte-range patches appended to the WAL. A transaction is durable when its commit record is durable; pool pages are written back later from RAM. This is the ordered-data variant of journaling: bulk data is written once and the log stays small.

### 4.2 WAL file

wal.db: page 0 is a header (magic KBWL, pool\_uuid, ring\_bytes, crc32c). The rest is a ring of ring\_bytes, which is your staging\_buffer\_bytes (default 64 MiB). The LSN is the byte position in the endless stream; the ring position is LSN modulo ring\_bytes.

**Record:** a 16-byte header (len u32, type u8, flags u8, tx\_id u16, lsn u64), the payload, then crc32c u32 over header and payload.

| Type | Name | Payload |
| --- | --- | --- |
| 1 | PATCH | file id u8 (0 = pool), page number u64, offset u16, length u16, bytes |
| 2 | INDEX\_OP | op u8 (insert or delete), key length (LEB128), key, uref u40 |
| 3 | INDEX\_RANGE\_BEGIN and INDEX\_RANGE\_END | kind u8 (move or delete), old prefix, new prefix |
| 4 | TX\_COMMIT | tx\_id, record count u32 |
| 5 | CHECKPOINT | pool\_lsn, index\_lsn |
| 6 | POOL\_GROW | new provisioned bytes u64 |
| 7 | PAD | fills the end of the ring |

### 4.3 Commit protocol

1. tx\_begin takes the writer lock (with a timeout).
2. Operations stage changes in RAM pages (marked dirty) and a patch list. Payload data is written to newly allocated space immediately with pwrite; it is not staged.
3. tx\_commit: (a) sync the pool file if data was written (can be shared with other commits); (b) append all PATCH and INDEX\_OP records plus TX\_COMMIT; (c) sync the WAL. The transaction is durable at this point.
4. Release the writer lock. Freed space becomes allocatable (section 2.4.5). Dirty pages stay in RAM for later write-back.

**Rules:** a dirty page may be written back only after the WAL is durable up to that page's last patch (the write-ahead rule). A transaction larger than half the ring is rejected with KB\_E\_TX\_TOO\_LARGE. The design keeps transactions small: moves and deletes are O(1) in the log (section 8).

### 4.4 Group commit and durability levels

Several transactions can share one sync: a commit waits at most group\_window (default 2 ms) for others, then one pair of syncs covers all of them. Levels: **FULL** (every commit synced; the default), **GROUP** (as above), **RELAXED** (sync every N ms; a crash may lose the last N ms but never corrupts), **NONE** (tests only).

### 4.5 Checkpoint and recovery

**Checkpoint:** when the ring is half full, when idle, and at close. Write back dirty pool pages in address order, sync the pool, append CHECKPOINT, update the superblock (wal\_checkpoint\_lsn and sequence), and let the ring reuse space below min(checkpoint LSN, index applied\_lsn). If the index lags and the ring fills, the index is forced to flush first (back-pressure).

**Recovery** (every open after an unclean shutdown):

1. Pick the valid superblock copy with the highest sequence number.
2. Scan the WAL from wal\_checkpoint\_lsn, validating each record's crc32c; stop at the first invalid or torn record.
3. Group records by transaction; keep only transactions that have a TX\_COMMIT.
4. Replay PATCH records in LSN order onto pool pages. Byte-range writes are idempotent, and a page torn by an interrupted write-back is repaired because every change since the checkpoint is in the log.
5. If index.db validates (uuid, crc32c, applied\_lsn inside the WAL), replay INDEX\_OP and range records after applied\_lsn; otherwise rebuild it.
6. Run the cheap verifier (superblock, segment headers, a sample of page checksums), set clean\_shutdown to 0 and continue.

Allocations of uncommitted transactions never reached the on-disk bitmap, so their space is still free: nothing leaks.

### 4.6 Invariants, verify and repair

| ID | Invariant | Repair |
| --- | --- | --- |
| I1 | Every LIVE header except the root has one parent whose child table holds exactly one entry for it with the same birth number | trust the child's parent\_ref for membership; rebuild the parent's entry |
| I2 | Parent links are acyclic and reach the root | report; quarantine under lost and found |
| I3 | birth\_num is below the parent's next\_child\_counter and unique among siblings | raise the counter; move duplicates to lost and found |
| I4 | child\_count equals the number of table entries | recount |
| I5 | The index has exactly one key per LIVE attached header, with the correct path and uref | rebuild the affected range |
| I6 | Every block, extent and run lies inside allocated pages; no two owners overlap | report; quarantine |
| I7 | Bitmap, kind map and summaries agree with reachability from headers and system structures | free confirmed leaks; report double use as corruption |
| I8 | Each fragment tag matches its owner's table entry | rewrite tag or entry from the owner |
| I9 | Frozen invariants hold (a frozen node implies flagged ancestors) | recompute flags |
| I10 | All checksums are valid | restore from the WAL if possible, else quarantine |
| I11 | DETACHED roots are listed in the reclaim queue | re-add |
| I12 | payload\_len equals the sum of extent used bytes and fits capacity | recompute from extents |

**Modes:** quick (at open), full (a user command; sequential read of all slabs), repair (applies the listed fixes inside transactions, always through the WAL).

### 4.7 Portability layer

All OS calls sit behind kb\_os\_\* functions.

| Operation | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Durable sync | fdatasync | fcntl F\_FULLFSYNC (plain fsync does not force the drive cache) | FlushFileBuffers |
| Preallocate | fallocate or posix\_fallocate | fcntl F\_PREALLOCATE, then ftruncate | SetEndOfFile |
| Truncate | ftruncate | ftruncate | SetEndOfFile |
| Uncached I/O (optional io\_mode=direct) | O\_DIRECT | fcntl F\_NOCACHE | FILE\_FLAG\_NO\_BUFFERING |
| Positioned I/O | pread, pwrite | pread, pwrite | overlapped ReadFile and WriteFile |

Direct I/O needs aligned buffers, offsets and lengths; the buffer pool already uses page-aligned buffers. The pool is never memory-mapped writable, so a failing disk returns an error code and not a SIGBUS.

## 5. Memory plane

### 5.1 Budget

max\_working\_ram\_bytes (call it M) is a hard ceiling for everything the engine allocates for data: page caches, payload windows, dirty metadata, index pages and the hole index. Code, thread stacks and module memory are outside the cap; modules are asked to report theirs, but it is not enforced.

| Pool | Default share of M | Floor | Notes |
| --- | --- | --- | --- |
| Index internal pages | pinned | min(32 MB, 5 percent) | always resident |
| Header and block page cache | 20 percent | 5 percent | slab pages, 63 headers each |
| Index leaf cache | 15 percent | 5 percent |  |
| Payload windows | the remainder | 20 percent | pinned or LRU |
| Dirty metadata (staging) | min(ring size, 10 percent) | none | bounded by the WAL ring (section 4.2) |

Shares adapt within the floors. If every byte is pinned, a request fails with KB\_E\_NO\_RAM. The cap is never exceeded.

### 5.2 Tiers

- **Tier 0:** index internal pages, the root and the system objects. Always resident.
- **Tier 1 (shallow):** header cores plus the aux summary. The unit of I/O is a slab page (4 KB, up to 63 headers). The viewport asks the index for a path range, gets urefs, sorts them by address, reads the pages in a batch and decodes. It draws from the header, module meta and thumbnail, and never runs a module.
- **Tier 2 (deep):** payload windows, pinned on demand (section 5.3).

### 5.3 Payload windows (finding C4)

Stitching a whole object into RAM cannot work for an object larger than the budget (a 50 GB video under a 16 GB cap). The unit is therefore a **window**: a byte range of the logical payload.

- The kernel maps the range onto extents (binary search over the prefix sums), reads each overlapped part with pread into one contiguous 64-byte-aligned buffer, and skips the 8-byte fragment tags. If one extent covers the window, it is a single read straight into the buffer, with no extra copy.
- Small objects are one window. Larger ones are read by modules in windows; payload\_read copies bytes without pinning.
- Sequential access is detected and read ahead (doubling up to a cap of 4 MB or 1 percent of M, whichever is smaller).
- Disk alignment does not matter to modules: the buffer is aligned by the pool.
- **Writes in V1** are whole-object replace and append. Patching a window of a very large object in place is V1.1; the recommended practice for large editable data is to split it into child objects.

### 5.4 Pins and leases

- Every pin returns a lease id. Unpinning is by lease, not by address, so two modules holding the same window release independently.
- A pinned buffer is never evicted, reused or moved. A background thread, a GPU queue or a DMA transfer may use it for as long as the lease is held.
- **Readers and writers do not block each other.** A writer works on its own buffer; the commit publishes a new version, and existing read leases keep pointing at the old buffer. Both versions count against M while they coexist.
- **Eviction order:** unpinned clean windows by LRU (a CLOCK approximation), then clean pages. Dirty metadata pages are written back, after the WAL is durable, before eviction.
- Header and aux data use leases too (aux\_get returns one).

### 5.5 Encryption (K9)

Structure (headers, blocks, index, WAL records) is never encrypted. Payload may be.

- **Read:** the loader reads extent bytes into the window buffer, then the crypto provider decrypts the buffer. In place is safe, because buffers are private RAM and not a shared file mapping, so plaintext cannot reach the disk through a dirty page.
- **Write:** the module's plaintext buffer is encrypted into a scratch buffer (never in place if the module still needs the plaintext) and written out of place.
- Framing (nonce, authentication tag) lives inside the payload bytes and belongs to the provider; keys are the provider's business.
- **V1 limitation:** providers operate on whole buffers, so an encrypted object is one window and must fit in RAM. A later revision can add a chunk size so windows align with authenticated chunks.
- Plaintext in RAM can be swapped out by the OS. The config option lock\_ram uses mlock or VirtualLock on buffers of encrypted objects.
- Partial encryption: the flag is per object; to encrypt only part of a document, split it into child objects.

### 5.6 Dirty data and write-back

- Metadata pages are dirty in RAM, covered by the WAL, and written back at checkpoints (write-ahead rule, section 4.3).
- Payload is written at commit, out of place. Payload buffers are clean afterwards, and eviction never writes payload.

### 5.7 Prefetch

The engine module issues header\_info and aux\_get calls ahead of need (for children of expanded nodes, for hook targets of visible objects, one hop deep by default). The pool treats them as ordinary tier-1 loads under the same budget; prefetch may use at most prefetch\_fraction of M (default 10 percent). Rev 1 of the ABI has no separate prefetch call.

### 5.8 OS cache and I/O modes

- **buffered** (default): the OS page cache sits under the pool. It is reclaimable and outside M, so total machine memory use can look larger than M.
- **direct:** bypasses the OS cache for exact accounting, and loses OS read-ahead (the pool does its own).
- Rebuild and verify scans advise sequential access to the OS.

## 6. Native format (type 0)

### 6.1 Purpose and scope

Scratch writing, notes, formatted text and images, rendered by the core without any module. The stored bytes are ASCII or narrow UTF-8 S-expressions, so extra structure costs few bytes. No audio or video. Links into other objects use spawn (a child) and xlink (a hook).

### 6.2 Grammar

```
document := '(' 'doc' attr* form* ')'
form     := list | string
list     := '(' symbol attr* form* ')'
attr     := ':' symbol value
value    := integer | string | symbol
string   := '"' chars '"'     ; chars may contain an escaped quote, backslash or n
symbol   := [a-z][a-z0-9-]*
integer  := [0-9]+
```

Whitespace separates tokens. Nesting depth is limited to 256.

### 6.3 Standard forms

| Form | Attributes | Meaning |
| --- | --- | --- |
| doc | :v version | root |
| sec | :id, :title | section |
| p | none | paragraph |
| b, i, code | none | inline emphasis |
| ul, li | none | list |
| img | :child N, :w, :h | image stored in child N (an image-typed object) |
| spawn | :anchor A, :child N, :label S | a child thread shown at this point |
| xlink | :anchor A, :text S | a cross-link; the target lives in the hook table |

Example (user children start at 16):

```
(doc :v 1
  (sec :id 1 :title "Architecture notes"
    (p "See the layout thread: " (spawn :anchor 10 :child 18 :label "Memory layout"))
    (p "Compare with " (xlink :anchor 11 :text "the recording at 00:12"))
    (img :child 19 :w 600 :h 400)))
```

### 6.4 Linking rules (C5)

- spawn :child N must name a child birth number of this object. The reference is soft: a missing child renders as a broken-reference marker, and deleting text never deletes the child. Children with no spawn in the text appear in a Detached list in the UI.
- xlink carries no target in the text. The target and locator live in the hook table, keyed by the anchor id (section 2.10). The raw view shows the target by consulting the hook.
- Anchor ids are unique per object, allocated from next\_anchor\_id in the aux directory, and never reused.
- img :child N points at a child whose type has an image handler.

### 6.5 Header synchronization

On every save, inside the same transaction as the payload write, the native handler:

1. parses the document;
2. collects anchors, spawn references and image references;
3. diffs them against the hook table and the child table and applies tx\_hook\_add and tx\_hook\_remove;
4. recomputes the summary (title from the first section title or first line, counts) and the optional thumbnail.

Parsing is a single-pass recursive descent, O(N). Large documents re-parse incrementally by top-level form. The recommended soft cap is 1 MB per type-0 object; larger texts should be split into child threads.

### 6.6 Editing model and offsets

The editor keeps the text in a piece table in RAM (module-internal). Anchors are ids, not byte offsets, so inserting text never invalidates a link and the header stores no text offsets. This resolves the offset-shifting problem raised in the review for type 0. Other types (a canvas) put coordinates inside the hook locator.

A save writes the whole object out of place, which is cheap below 1 MB. Log-style threads use tx\_append\_payload.

### 6.7 Failure behaviour

If the text does not parse, the save is still accepted (the user may save broken text). The viewer shows raw text with the error position, the header sync is skipped, and module flag bit 16 (NEEDS\_SYNC) is set until a later valid save.

## 7. Module plane

### 7.1 Roles

| Role | Does | Must not |
| --- | --- | --- |
| Kernel | storage, index, WAL, buffer pool, type dictionary, tree structure | interpret payloads or graph semantics |
| Engine module | prefetch, background maintenance scheduling, cross-module events | bypass transactions |
| GUI module | draw tier-1 data, request payload windows on activation | hold transactions open across user input |
| Type handler | summaries, locator resolution, renumber and reclaim hooks | write structure except through transactions |
| Graph module | owns cyclic and secondary graphs (K10), stores edges in its own objects or hooks | assume the kernel tracks its edges |
| Crypto provider | encrypt and decrypt payload buffers | touch structure |

**Guarantee:** structure traversal, movement and deletion work for objects whose module is missing, because structure belongs to the kernel.

### 7.2 Loading and manifests

- Modules are shared libraries in module\_dir. Each exports the symbol kb\_module\_entry, returning a kb\_module\_t.
- The loader checks abi\_rev, accepts any struct\_size at least as large as it knows, and reads a sidecar text manifest: name, uuid, version, abi\_rev, requested type ids, required module uuids.
- Load order follows dependencies. on\_load receives the kernel API table, valid until on\_unload returns, and may register type handlers and crypto providers, subscribe to events and start threads.
- on\_unload must join its threads and release its leases. Unloading with leases outstanding fails with KB\_E\_BUSY.
- **Versioning:** the API table only grows at the end. A module checks struct\_size before using a newer entry. A breaking change bumps abi\_rev.

### 7.3 Threading contract

- All kernel calls are thread-safe unless noted. A transaction belongs to the thread that began it. Only one transaction is open at a time; tx\_begin waits up to its timeout, then returns KB\_E\_BUSY.
- **Never hold a transaction while waiting** on user input, disk, network or GPU work. Prepare outside, apply inside.
- Event callbacks run on a kernel notification thread, never inside a transaction. They must be quick and must not call tx\_begin synchronously; post the work to your own thread.
- Handler callbacks: extract\_summary runs on the committing thread before the commit; on\_child\_renumber runs inside the transaction; on\_reclaim runs on the background reclaim worker and may open its own short transactions.
- Modules may create threads and use GPUs and devices freely. There is no sandbox: a module crash ends the process, and the WAL keeps the pool consistent (section 4).
- Pointers from payload\_pin and aux\_get stay valid until the matching unpin or release. Modules never free kernel memory.

### 7.4 Transaction semantics

- tx\_create returns the child reference and its birth number. tx\_write\_payload replaces the whole payload; tx\_append\_payload appends; tx\_set\_aux replaces meta and thumbnail; hooks use tx\_hook\_add and tx\_hook\_remove.
- tx\_move and tx\_delete are O(1) in the log (section 8). tx\_delete performs phase 1 only; reclaim runs in the background.
- tx\_swap\_birth requires the parent's type handler to implement on\_child\_renumber (it rewrites payload references inside the transaction). Without a handler the swap is refused (KB\_E\_TYPE\_UNKNOWN).
- After most errors the transaction stays usable; a failed tx\_commit aborts everything. Calls are not idempotent: re-read state before retrying.

### 7.5 The C header (revision 1)

Design choices behind it: modules see decoded views, never the disk layout, so the format can evolve; references carry a generation; unpinning is by lease; payload access is by window; every struct that crosses the boundary has a size field; all calls return a status code. This text compiles clean with gcc -std=c11 -Wall -Wextra -pedantic -Werror, and includes cleanly from C++17. On a 64-bit target the sizes are: kb\_ref 16, kb\_header\_info 80, kb\_hook 32, kb\_view 64, kb\_event 48, kb\_pool\_stats 88 bytes.

```c
/* kb_abi.h - KB Core module ABI, revision 1 (C11). Modules never see on-disk layouts. */
#ifndef KB_ABI_H
#define KB_ABI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KB_ABI_REV         1u
#define KB_MAX_PATH_BYTES  1024u
#define KB_MODULE_ENTRY    "kb_module_entry"

typedef int32_t  kb_status_t;
typedef uint64_t kb_tx_t;      /* 0 = invalid                                  */
typedef uint64_t kb_lease_t;   /* 0 = invalid; names exactly one pin           */

enum {
    KB_OK = 0,
    KB_E_BAD_ARG = -1,   KB_E_NOT_FOUND = -2,  KB_E_EXISTS = -3,
    KB_E_FROZEN = -4,    KB_E_CYCLE = -5,      KB_E_DEPTH = -6,
    KB_E_LIMIT = -7,     KB_E_NO_RAM = -8,     KB_E_NO_SPACE = -9,
    KB_E_IO = -10,       KB_E_CORRUPT = -11,   KB_E_BUSY = -12,
    KB_E_TX_TOO_LARGE = -13, KB_E_PINNED = -14, KB_E_READONLY = -15,
    KB_E_TYPE_UNKNOWN = -16, KB_E_UNSUPPORTED = -17, KB_E_STALE_REF = -18,
    KB_E_TOO_FRAGMENTED = -19, KB_E_INDEX_BUSY = -20
};

/* Object reference: header slot address + slot generation (ABA guard). */
typedef struct kb_ref {
    uint64_t addr;   /* byte offset of the 64-byte header in pool.db; 0 = null */
    uint32_t gen;    /* slot generation when the reference was made            */
    uint32_t _pad;
} kb_ref_t;

enum {
    KB_F_PATH_FROZEN = 1u << 0,   KB_F_HAS_FROZEN_BELOW = 1u << 1,
    KB_F_OBJ_IMMUTABLE = 1u << 2, KB_F_PAYLOAD_ENCRYPTED = 1u << 3,
    KB_F_HAS_AUX = 1u << 4,       KB_F_HAS_PAYLOAD_CRC = 1u << 5,
    KB_F_SYSTEM = 1u << 10        /* bits 16..31: module-defined, preserved */
};

typedef struct kb_header_info {          /* decoded copy, not the disk layout */
    uint32_t struct_size;
    uint32_t flags;
    kb_ref_t self;
    kb_ref_t parent;
    uint64_t payload_len;
    uint32_t birth_num, child_count, change_seq;
    uint32_t aux_hook_count, aux_meta_len, aux_thumb_len;
    uint16_t type_id;
    uint8_t  type_version;
    uint8_t  state;
} kb_header_info_t;

typedef struct kb_hook {                 /* kernel-visible cross-link hint */
    kb_ref_t       target;
    uint32_t       anchor_id;
    uint16_t       graph_type_id;
    uint8_t        flags;
    uint8_t        locator_len;          /* 0..255, opaque to the kernel   */
    const uint8_t *locator;              /* valid while the lease is held  */
} kb_hook_t;

enum { KB_VIEW_WRITABLE = 1u << 0, KB_VIEW_ALIGN64 = 1u << 1, KB_VIEW_VERIFY_CRC = 1u << 2 };

typedef struct kb_view {                 /* contiguous RAM window of a payload */
    uint32_t   struct_size;
    uint32_t   flags;
    kb_ref_t   object;
    uint64_t   offset;                   /* window start in the logical payload */
    uint64_t   length;                   /* valid bytes in the window           */
    uint64_t   capacity;                 /* writable bytes available at data    */
    uint8_t   *data;                     /* 64-byte aligned                     */
    kb_lease_t lease;
} kb_view_t;

enum {
    KB_EV_CREATED = 1u << 0, KB_EV_DELETED = 1u << 1, KB_EV_MOVED = 1u << 2,
    KB_EV_PAYLOAD = 1u << 3, KB_EV_AUX = 1u << 4,     KB_EV_FROZEN = 1u << 5,
    KB_EV_MEM_PRESSURE = 1u << 6, KB_EV_INDEX_STATE = 1u << 7
};

typedef struct kb_event {
    uint32_t kind;
    uint32_t _pad;
    kb_ref_t object;
    kb_ref_t other;                      /* e.g. old parent for KB_EV_MOVED */
    uint64_t value;                      /* e.g. bytes to free for pressure */
} kb_event_t;

typedef struct kb_pool_stats {
    uint32_t struct_size, page_size;
    uint64_t file_bytes, hwm_bytes, free_below_hwm, largest_hole;
    uint64_t object_count, ram_limit, ram_used, wal_used, wal_total;
    uint32_t index_state;                /* 0 valid, 1 repairing, 2 rebuilding */
    uint32_t _pad;
} kb_pool_stats_t;

typedef bool (*kb_path_cb)(const uint8_t *path, uint32_t path_len, kb_ref_t ref, void *ctx);
typedef bool (*kb_child_cb)(uint32_t birth_num, kb_ref_t child, void *ctx);
typedef bool (*kb_ref_cb)(kb_ref_t ref, void *ctx);
typedef bool (*kb_hook_cb)(const kb_hook_t *hook, void *ctx);
typedef void (*kb_event_cb)(const kb_event_t *ev, void *ctx);

typedef struct kb_type_handler {
    uint32_t struct_size;
    uint16_t type_id;
    uint8_t  version, _pad;
    kb_status_t (*extract_summary)(const uint8_t *payload, uint64_t len,
                                   uint8_t *meta, uint32_t *meta_len,
                                   uint8_t *thumb, uint32_t *thumb_len);
    kb_status_t (*on_child_renumber)(kb_tx_t tx, kb_ref_t parent,
                                     uint32_t from_birth, uint32_t to_birth);
    kb_status_t (*resolve_locator)(kb_ref_t obj, const uint8_t *loc,
                                   uint32_t loc_len, void *out_module_view);
    void        (*on_reclaim)(kb_ref_t obj);
} kb_type_handler_t;

typedef struct kb_crypto_provider {
    uint32_t struct_size;
    uint16_t provider_id, _pad;
    void    *ctx;
    kb_status_t (*encrypt)(void *ctx, const uint8_t *plain, uint64_t plain_len,
                           uint8_t *out, uint64_t *out_len);
    kb_status_t (*decrypt)(void *ctx, const uint8_t *in, uint64_t in_len,
                           uint8_t *plain, uint64_t *plain_len);
} kb_crypto_provider_t;

typedef struct kb_kernel_api {
    uint32_t struct_size, abi_rev;

    /* registration */
    kb_status_t (*register_type)(const kb_type_handler_t *h, const char *name);
    kb_status_t (*register_crypto)(const kb_crypto_provider_t *p);

    /* naming and traversal (path bytes are the canonical path_uid) */
    kb_status_t (*resolve_path)(const uint8_t *path, uint32_t len, kb_ref_t *out);
    kb_status_t (*get_path)(kb_ref_t obj, uint8_t *buf, uint32_t cap, uint32_t *out_len);
    kb_status_t (*is_ancestor)(kb_ref_t a, kb_ref_t b, bool *out);
    kb_status_t (*children)(kb_ref_t parent, kb_child_cb cb, void *ctx);
    kb_status_t (*subtree_scan)(const uint8_t *prefix, uint32_t prefix_len,
                                uint32_t max_depth, uint64_t max_objects,
                                kb_path_cb cb, void *ctx);
    kb_status_t (*ledger_by_type)(uint16_t type_id, kb_ref_cb cb, void *ctx);

    /* shallow access: headers, summaries, hooks */
    kb_status_t (*header_info)(kb_ref_t obj, kb_header_info_t *out);
    kb_status_t (*aux_get)(kb_ref_t obj, const uint8_t **meta, uint32_t *meta_len,
                           const uint8_t **thumb, uint32_t *thumb_len, kb_lease_t *lease);
    kb_status_t (*hooks_iter)(kb_ref_t obj, kb_hook_cb cb, void *ctx);
    void        (*aux_release)(kb_lease_t lease);

    /* deep access: payload windows (never requires the whole object in RAM) */
    kb_status_t (*payload_read)(kb_ref_t obj, uint64_t offset, void *dst,
                                uint64_t len, uint64_t *out_read);
    kb_status_t (*payload_pin)(kb_ref_t obj, uint64_t offset, uint64_t len,
                               uint32_t flags, kb_view_t *out);
    void        (*payload_unpin)(kb_lease_t lease);

    /* transactions: one writer at a time; keep them short */
    kb_status_t (*tx_begin)(uint32_t timeout_ms, kb_tx_t *out);
    kb_status_t (*tx_create)(kb_tx_t tx, kb_ref_t parent, uint16_t type_id,
                             kb_ref_t *out_child, uint32_t *out_birth);
    kb_status_t (*tx_write_payload)(kb_tx_t tx, kb_ref_t obj, const kb_view_t *whole);
    kb_status_t (*tx_append_payload)(kb_tx_t tx, kb_ref_t obj, const void *src, uint64_t len);
    kb_status_t (*tx_set_aux)(kb_tx_t tx, kb_ref_t obj,
                              const uint8_t *meta, uint32_t meta_len,
                              const uint8_t *thumb, uint32_t thumb_len);
    kb_status_t (*tx_hook_add)(kb_tx_t tx, kb_ref_t src, const kb_hook_t *hook);
    kb_status_t (*tx_hook_remove)(kb_tx_t tx, kb_ref_t src, uint32_t anchor_id);
    kb_status_t (*tx_move)(kb_tx_t tx, kb_ref_t obj, kb_ref_t new_parent);
    kb_status_t (*tx_swap_birth)(kb_tx_t tx, kb_ref_t parent, uint32_t a, uint32_t b);
    kb_status_t (*tx_freeze)(kb_tx_t tx, kb_ref_t obj, bool freeze);
    kb_status_t (*tx_delete)(kb_tx_t tx, kb_ref_t obj);
    kb_status_t (*tx_commit)(kb_tx_t tx);
    void        (*tx_abort)(kb_tx_t tx);

    /* events and diagnostics */
    kb_status_t (*subscribe)(uint32_t mask, kb_event_cb cb, void *ctx, uint64_t *out_sub);
    void        (*unsubscribe)(uint64_t sub);
    kb_status_t (*pool_stats)(kb_pool_stats_t *out);
} kb_kernel_api_t;

enum { KB_CAP_TYPE = 1u << 0, KB_CAP_GRAPH = 1u << 1, KB_CAP_CRYPTO = 1u << 2,
       KB_CAP_GUI = 1u << 3,  KB_CAP_ENGINE = 1u << 4 };

typedef struct kb_module {
    uint32_t    struct_size, abi_rev;
    const char *name;
    uint8_t     uuid[16];
    uint32_t    version, capabilities;
    kb_status_t (*on_load)(const kb_kernel_api_t *api, void **module_ctx);
    void        (*on_unload)(void *module_ctx);
} kb_module_t;

typedef const kb_module_t *(*kb_module_entry_fn)(void);

#ifdef __cplusplus
}
#endif
#endif /* KB_ABI_H */
```

### 7.6 Optional services (not in revision 1 of the header)

- **Ordered-table service (ABI revision 2, phase 5):** named, persistent, ordered key-value tables inside pool.db with variable-length keys and values, transactional through the same WAL, built on the B+tree code that already serves child trees. Graph modules would use it for edge indexes, backlinks, per-type lists and search postings. Until then, modules keep their own data inside module-typed objects, which works but gives no shared index machinery.
- **Backlink index (feature flag, default off):** a derived reverse index of hooks (target to source, about 16 bytes per link) maintained by the kernel from tx\_hook\_add and tx\_hook\_remove. At two links per object and a billion objects it would cost about 32 GB, which is why it is optional.

### 7.7 Events

| Event | object | other | value |
| --- | --- | --- | --- |
| KB\_EV\_CREATED, KB\_EV\_DELETED | the object | none | none |
| KB\_EV\_MOVED | the object | old parent | none |
| KB\_EV\_PAYLOAD, KB\_EV\_AUX | the object | none | change\_seq |
| KB\_EV\_FROZEN | the object | none | 1 frozen, 0 unfrozen |
| KB\_EV\_MEM\_PRESSURE | none | none | bytes the pool wants back |
| KB\_EV\_INDEX\_STATE | none | none | 0 valid, 1 repairing, 2 rebuilding |

Delivery is ordered per subscriber and best effort; payload and aux events may coalesce per object (compare change\_seq to see what you missed). On memory pressure, modules should drop caches.

### 7.8 Type handler duties

- **extract\_summary:** a deterministic function of the payload bytes. Keep it fast (a few milliseconds typical). Meta should stay under 4 KB and the thumbnail under 64 KB.
- **on\_child\_renumber:** rewrite the parent's payload references inside the transaction, or return an error so the swap is refused.
- **resolve\_locator:** interpret a hook locator (a time range, a byte span, a rectangle) for the target type.
- **on\_reclaim:** called for each object of the module's types during background reclaim. Drop module-owned edges and caches. It may open short transactions of its own.
- **Stale links:** a reference to a deleted object fails with KB\_E\_STALE\_REF or KB\_E\_NOT\_FOUND thanks to the slot generation. Graph modules clean such edges lazily or in on\_reclaim.

### 7.9 The engine and GUI modules

The **engine module** schedules prefetch, background maintenance (reclaim, index repair, checkpoint hints) and cross-module events. The **GUI module** reads tier-1 data only, lists regions with subtree\_scan, draws thumbnails, and asks for payload windows when an object is activated. Both are replaceable, because the kernel never depends on either.

## 8. Operations and state machines

Every operation below runs inside one transaction (section 4.3) unless it says otherwise. Log sizes are rough.

### 8.1 Create an object (tx\_create)

**Preconditions:** parent is LIVE and not OBJ\_IMMUTABLE (else KB\_E\_READONLY); depth would be at most 1023; the path would be at most 1024 bytes; the parent's birth counter is below 2^32 (else KB\_E\_LIMIT).

1. Allocate a header slot, preferring the run that holds the parent's latest child (section 2.4.4).
2. birth = parent.next\_child\_counter; increment the counter.
3. Write the header core: state LIVE, parent\_ref, birth\_num, type, flags, the slot's kept slot\_gen, crc32c.
4. Add the entry to the parent's child table (inline, array or tree; grow it if full, section 2.7); update the parent's child\_count, children\_ref, representation bits, change\_seq and crc32c.
5. Derive the path (parent path plus the component code for birth) and log an INDEX\_OP insert.
6. Commit. Log: about five patches and one index record, roughly 300 bytes.

### 8.2 Payload replace and append

- **Replace:** (1) call the type handler's extract\_summary on the new bytes; (2) allocate new extents with slack; (3) write tag and data with pwrite; (4) sync the pool; (5) in the transaction patch the core (ext0 or extent table, payload\_len, change\_seq), write the new aux block, and free the old extents and old aux (pending); (6) commit.
- **Append in place:** if the last extent has spare capacity, write the bytes there, sync the pool, then commit the new used length and payload\_len. Otherwise allocate a new extent (section 2.9).
- **Encrypted objects:** encrypt into scratch before step 3 (section 5.5).

### 8.3 Delete (two phases, N2)

**Phase 1** (one small transaction, O(1) whatever the subtree size):

1. Reject if X is the root, is frozen, or has HAS\_FROZEN\_BELOW (KB\_E\_FROZEN).
2. Remove X's entry from the parent's child table; set X.state to DETACHED.
3. Append (uref, generation) of X to the reclaim queue (system object 3).
4. Log INDEX\_RANGE\_BEGIN (delete, path of X), commit, then drop the index range and log INDEX\_RANGE\_END.

From this moment the object is gone for readers: index lookups validate the header state, and a DETACHED header counts as not found.

**Phase 2** (background reclaim worker, many small transactions of at most 256 headers each):

1. Take a queue entry and walk the detached subtree in post-order (children first).
2. For each child, validate before touching it: its parent\_ref equals the parent, its birth\_num equals the table entry, and its state is LIVE or DETACHED. A slot that was freed and reused by a new object fails this check, so the worker can never free someone else's object.
3. Call on\_reclaim for objects of module types (outside the transaction), then free extents, aux, child blocks and the header slot (slot\_gen increments, state FREE).
4. Remove freed entries from the detached parent's table in the same transaction.
5. When the subtree root itself is freed, remove its queue entry.

Crashes between batches resume from the queue entry; nothing is half-freed inside a batch.

### 8.4 Move a subtree (tx\_move)

**Preconditions:** X is not the root, not frozen, with no frozen node below; the new parent Q is LIVE and not OBJ\_IMMUTABLE; Q is not inside X's subtree (walk up from Q comparing with X, O(depth)); the moved subtree still fits the depth and path limits (taken from the longest key in the index range, so the index must be valid; if it is repairing the call returns KB\_E\_INDEX\_BUSY).

1. Remove X's entry from the old parent P; decrement P.child\_count.
2. birth' = Q.next\_child\_counter; increment the counter; set X.birth\_num = birth' and X.parent\_ref = Q.
3. Append (birth', X) to Q's table; increment Q.child\_count.
4. Log the header and table patches and INDEX\_RANGE\_BEGIN (move, old prefix, new prefix). Commit. The truth plane is now final.
5. Outside the writer lock, under a lock on the two key ranges, perform the index range move (section 3.3), log INDEX\_RANGE\_END and release the range lock. Lookups under either prefix return KB\_E\_INDEX\_BUSY meanwhile.
6. Emit KB\_EV\_MOVED.

**Cost:** three header patches and two table patches, about 400 bytes of log. Index work is O(N/B) pages for N keys. Descendants are not touched.

**Note:** old paths are never reused (births are never reused), and a path held outside the system breaks when its target moves. Freeze it (K6) or hold a reference instead.

### 8.5 Swap two sibling birth numbers (tx\_swap\_birth)

**Preconditions:** both children exist; neither is frozen nor has a frozen node below; the parent is mutable; the parent's type handler implements on\_child\_renumber.

1. Call on\_child\_renumber, which rewrites the parent's payload references (spawn :child a and b exchange) as a payload write in this transaction.
2. Swap birth\_num in the two child headers and exchange their entries in the parent's table.
3. Log INDEX\_RANGE\_BEGIN (swap, path a, path b) and commit.
4. Apply the swap in the index as three range moves through a temporary prefix, then log INDEX\_RANGE\_END. Recovery re-derives both ranges.

Truth cost is O(1); index cost is O(N of both subtrees / B).

### 8.6 Freeze and unfreeze (tx\_freeze)

- **Freeze:** walk up from X to the root, set PATH\_FROZEN on X and HAS\_FROZEN\_BELOW on every ancestor (O(depth) header patches in one transaction).
- **Unfreeze:** clear PATH\_FROZEN on X, then walk up and clear HAS\_FROZEN\_BELOW on any ancestor with no remaining frozen child (found by scanning that ancestor's child headers).

### 8.7 Open and evict

Open means pin a window (section 5.3): map the range to extents, read, decrypt if flagged, hand out the lease. Eviction follows section 5.4. Neither writes to disk.

### 8.8 Startup and shutdown

**Startup:** read kb.conf; open the pool and pick a superblock; if clean\_shutdown is 0, run WAL recovery (section 4.5); open the index and validate it, or start a rebuild in the background; load the type dictionary; load modules in dependency order; start the engine and GUI modules. While the index rebuilds, reference-based and child-table navigation still work, because the truth is intact; only path lookups wait.

**Shutdown:** stop accepting transactions; wait for open ones; checkpoint; flush the index and stamp applied\_lsn; set clean\_shutdown to 1; sync.

### 8.9 Pool growth and shrink

See section 2.4.6. Growth runs automatically from the allocator; shrink runs only on the user's command.

### 8.10 Crash matrix

| Operation | Crash before the commit is durable | Crash after commit, before write-back or index update | Crash inside an index range operation |
| --- | --- | --- | --- |
| Create | no object | patches and index record replayed | not applicable |
| Payload replace | old payload intact; new extents are free space | pointer swap replayed | not applicable |
| Append in place | old length; spare bytes ignored | new length replayed | not applicable |
| Delete phase 1 | object still present | object detached, queue entry present, reclaim resumes | index range re-derived (the delete is idempotent) |
| Reclaim batch | batch not applied | resume with the next batch; validation protects reused slots | not applicable |
| Move | not moved | moved in the truth plane | both ranges re-derived by walking the moved subtree |
| Swap | not swapped | swapped in the truth plane | both ranges re-derived |
| Freeze | not frozen | frozen | not applicable |
| Pool growth | file may be longer; the superblock keeps the old size | new pages free | not applicable |
| Checkpoint | the WAL still holds the changes | one valid superblock copy survives | not applicable |

## 9. Performance and footprint

All figures are estimates from the layout arithmetic, to be measured in phase 7.

### 9.1 Per-object footprint

Total = 64-byte header + payload allocation (class or pages, tag included) + aux + about 9 bytes of index + a 10-byte entry in the parent's child table. Slab rounding of aux blocks (up to 20 percent) is not counted.

| Object | Payload | Allocated | Aux | Total | Overhead |
| --- | --- | --- | --- | --- | --- |
| empty marker | 0 | 0 | 0 | 83 B | not meaningful |
| 200 B note | 200 B | 256 B | 0 | 339 B | 70 percent |
| 2 KB note | 2,000 B | 2,048 B | 0 | 2,131 B | 6.5 percent |
| 8 KB document | 8,192 B | 10,240 B | 200 B | 10,523 B | 28.5 percent |
| 100 KB document | 100,000 B | 102,400 B | 400 B | 102,883 B | 2.9 percent |
| 5 MB image | 5,000,000 B | 5,001,216 B | 20,000 B | 5,021,299 B | 0.4 percent |

- **A billion tiny objects:** headers 64 GB, index 9 GB, child entries 10 GB, so about 83 GB of structure.
- **The V1 draft under the same count:** by my count about 160 bytes for an empty object at depth 10 (96-byte header, the path stored in the header and again in the parent's 32-byte child entry, plus its index key), so roughly twice as much.
- The 8,192-byte document shows the tag effect: 8,200 bytes needed, so the next class is used.

### 9.2 Operation costs

| Operation | Cold pages touched | Syncs | Notes |
| --- | --- | --- | --- |
| Resolve a path | about 1 index leaf plus 1 header page | 0 | internal index pages are resident |
| Header by reference | 1 page | 0 | one page holds up to 63 headers |
| List children | the child block plus adjacent header pages | 0 | siblings are placed together |
| Create | 0 if the parent is hot | 1, shareable | about 300 bytes of log |
| Read a window | usually one read | 0 | one extent, one read |
| Append in place | 1 write | 2, shareable | data sync, then log sync |
| Move | none cold | 1, shareable | index work in the background |
| Delete | none cold | 1, shareable | reclaim in the background |
| Rebuild the index | the header slabs | 0 | about 64 GB read per billion objects |

**Sync cost** dominates writes. A device sync commonly takes 0.1 to 1 ms on NVMe, which puts unbatched commits near 1,000 to 10,000 per second; group commit of about 100 transactions per sync would put it above 100,000 per second. These are illustrative figures to measure, not promises.

### 9.3 Levers

- group\_window and the durability level (section 4.4).
- Affinity placement, which keeps families on shared pages.
- Page size 16 KB (255 headers per header page, larger index fan-out), chosen at format time.
- Direct I/O when exact RAM accounting matters.
- Hardware CRC32C and ordered-data commits, so bulk payload is written once.

### 9.4 Trade-offs taken

| Choice | Gain | Cost |
| --- | --- | --- |
| 64-byte header with no path | smaller, moves are O(1) in the truth plane | a cold path derivation reads up to depth pages |
| Prefix-length varint | correct byte order at every size | forward parsing only |
| Out-of-place payload writes | atomic, written once | temporary space amplification; fragmentation until the defragmenter exists |
| One writer | simple correctness | write concurrency is limited, acceptable for a single user |
| No mmap | hard RAM cap, errors instead of SIGBUS | one extra copy under buffered I/O (direct I/O removes it) |
| Derived index | small log, rebuildable | rebuild time after a loss |
| Slab ladder | dense small objects | up to 20 percent internal waste |

## 10. Audit and traceability

### 10.1 Defects found in the pasted draft

| # | Finding | Impact | Fix here |
| --- | --- | --- | --- |
| 1 | Header called expandable, but its growable tables sit inside an immovable header; a capacity field with no growth rule | growth would move the header, which every reference points at | fixed core plus blocks (2.7) |
| 2 | Whole-object stitching into RAM | impossible for objects larger than the budget | windows (5.3) |
| 3 | path\_uid stored in the object's header, again in the parent's child entry, and in the index; a move rewrites N headers and needs a dirty flag | about three copies of every path, O(N) truth writes, a resumable cascade to get right | relative identity (2.6) |
| 4 | 7+1 claimed to sort correctly under memcmp | order breaks above 16,383 children per parent | prefix-length varint (1.4) |
| 5 | Cascade described as O(N); each index update is O(log N) | N log N when done key by key | range move, O(N/B) (3.3) |
| 6 | Backlink table dropped when hooks were introduced, yet header relocation needs reverse references | relocation could not be made safe | headers immovable in V1, stubs in V1.1, optional backlink index (7.6) |
| 7 | Index sized at a few MB for ten million objects | about 20 times too small (about 90 MB) | 3.4 |
| 8 | Hook locator fixed at 28 bytes | too small for a canvas rectangle with zoom | variable locator up to 255 bytes (2.10) |
| 9 | Raw target address written into the S-expression text | duplicates the truth; text and header can disagree | anchor ids only (6.4) |
| 10 | ABI header used types it never defined, misspelt a field (tx\_commitPayload), put size\_t in an ABI, had no error codes or versioning, exposed a raw disk struct, unpinned by address | not usable as a contract | revised header (7.5) |
| 11 | WAL described as a RAM-backed log with no record format, ordering or recovery | no crash guarantee | section 4 |
| 12 | dirty\_subtree\_move flag costs header writes per move | exists only because of the cascade | removed (2.6) |
| 13 | Global type ledger table | 5 or more bytes per object | page summaries (2.11) |
| 14 | Allocator hand-waved; hole punching and truncation included | violates K12 | 2.4 |
| 15 | No concurrency model, limits, system objects, portability notes, verifier or tests | gaps | 3.6, 1.4.5, 2.11, 4.7, 4.6, 11 |
| 16 | Stale references could silently reach a different object | wrong-object access | slot generation (2.6) |

### 10.2 Trace to the first reviewer's items

| Item | Resolution | Section |
| --- | --- | --- |
| A: extent lists versus zero-copy pointers | modules get contiguous RAM windows; disk layout is invisible | 5.3, 2.9 |
| B: what pointers store | parent\_ref, child\_ref and fragment owner\_ref are 40-bit urefs; hooks store uref plus generation (address plus generation in the ABI); frozen and anchor references store nothing, because frozen is a flag and paths are derived | 2.6, 2.10 |
| C: raw mmap versus the RAM cap | no mmap; explicit buffer pool | 5 |
| D: mmap writes versus crash consistency | WAL with data-first commit | 4 |
| U1: path\_uid encoding | prefix-length varint | 1.4 |
| U2: child position anchoring | anchor ids, not offsets | 6.4, 6.6 |
| U3: pointer lifetime and compaction safety | private RAM buffers and leases; relocation in V1.1 | 5.4 |
| U4: secondary graphs, locators, ledger | module-owned graphs; hooks with opaque locators; ledger by page summaries; optional backlink index | 2.10, 2.11, 7.6 |
| Q1: language and OS | C11; Linux, macOS and Windows through the portability layer; targets pending | 4.7, 11.1 |
| Q2: internal pointer type | 40-bit uref | 1.2 |
| Q3: path encoding and sibling insertion | prefix-length varint; creation-order births, visual order separate, so no relabeling on insertion | 1.4 |
| Q4: fragmented payload access | windows | 5.3 |
| Q5: primary thread format | S-expression with anchor ids | 6 |
| Q6: viewport and RAM cap | hard cap through the explicit pool; no refresh button | 5 |
| Q7: relocation safety and crash consistency | WAL and leases; no raw pointers into the disk layout | 4, 5.4 |
| Q8: ledger and secondary graph storage | page summaries; module-owned graphs; optional table service and backlinks | 7.6 |

### 10.3 Trace to your own later points

| Your point | Status | Section |
| --- | --- | --- |
| Header carries mutability flags, type, extent layout, module meta, thumbnail | included | 2.6, 2.9, 2.10 |
| Native binary format as S-expressions, image and formatted text only | included | 6 |
| Index in a separate file, truth in the object network | included | 3 |
| User-sized buffer so a crash leaves the old or the new state | included (the WAL ring) | 4 |
| Header stores the child's path\_uid | changed (C2), revert path given | 2.6 |
| 7+1 encoding | changed (C1), revert path given | 1.4.6 |
| Nothing returned to the OS unless asked | included | 2.4.6 |
| Hole management reusing freed space first | included | 2.4 |
| Expandable headers | included | 2.7 |
| Minimap and defragmenter deferred | respected | 11.2 |
| Cyclic graphs module-specific; C core; GUI and engine as modules | included | 7 |

## 11. Open decisions, roadmap, tests, risks

### 11.1 Decisions needed from you

1. **C2:** confirm relative-only headers. If you want absolute paths stored in headers, say so and the revert design from section 2.6 becomes the plan.
2. **C1:** confirm the prefix-length varint, or keep 7+1 with a cap of 16,383 children per parent.
3. **Platforms:** macOS (16 KB pages), Linux, Windows? Default page size 4 KB or 16 KB?
4. **Birth numbers:** is u32 enough (4.29 billion children over one parent's lifetime)?
5. **Transactions:** single writer with a timeout (my default) or optimistic concurrent transactions.
6. **Services:** build the ordered-table service and the backlink index in the first release, or later in ABI revision 2?
7. **I/O mode:** buffered (my default) or direct.
8. **Header:** keep the 64-byte core with no spare bytes, or trade some payload\_len bits for a spare field.

### 11.2 Roadmap (bottom-up; each phase is testable alone)

| Phase | Content |
| --- | --- |
| 0 | Freeze this spec; write test vectors (path codes, header images, WAL records, slab tables) |
| 1 | Path library: component codec, comparator, operations; property tests (order, prefix, range, round trip), already prototyped in the verification script |
| 2 | Platform layer, pool file, superblock, segment maps, page cache, allocators, WAL ring, commit and recovery; fault-injection harness |
| 3 | Object model: header codec, child, extent and aux blocks; create, payload, delete, move, freeze; the invariants verifier |
| 4 | index.db: B+tree with front coding, range move and delete, rebuild, bind and validate |
| 5 | Buffer pool: budgets, tiers, windows, leases, eviction, encryption hook |
| 6 | Module loader, ABI table, type 0 handler (parser and sync), minimal GUI and engine modules |
| 7 | Verify and repair tool; benchmarks and tuning of the constants (page size, thresholds, ladder, group window) |
| 8 (deferred) | Relocation with forwarding stubs, defragmenter, minimap, ordered-table service |

### 11.3 Test strategy

| Layer | Method |
| --- | --- |
| Path codec | property-based tests against a tuple-order reference model |
| Allocator | model-based test against a simple interval set; random alloc and free; checks for no overlap and lowest-first |
| WAL and recovery | crash injection through a block-device shim that drops, reorders and tears writes; after each crash, recover, run the full verifier, and compare with an in-memory reference of the committed state |
| Tree operations | random sequences of create, move, delete, swap and freeze against a reference tree; verify I1 to I12 after every step |
| Index | compare against a sorted-map reference; rebuild must equal the incrementally maintained index |
| S-expression parser | grammar-based fuzzing, depth and size limits, print-and-parse round trips |
| Concurrency | multiple readers plus the writer under ThreadSanitizer; leases and eviction under pressure; the RAM cap asserted against accounting |
| Soak | days-long randomized workload with periodic kill -9 and restart |
| Performance | the benchmarks of section 9 on reproducible datasets of 10^6, 10^7 and 10^8 objects |

### 11.4 Top risks

1. **Crash-consistency bugs** (highest). Mitigation: fault injection from phase 2.
2. **Index and truth diverging.** Mitigation: the verifier, re-derivation, and never trusting the index for correctness.
3. **Too many features at once.** Mitigation: the phases; each one is usable.
4. **Module ABI churn.** Mitigation: struct\_size and revision discipline, and a small API until phase 6 settles.
5. **Unverified performance assumptions.** Every number here is an estimate; measure in phase 7 and tune.
6. **In-process modules without isolation.** A module bug can end the process. The WAL protects the data; an optional watchdog can come later.
