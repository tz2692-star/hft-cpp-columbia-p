# Flat, price-indexed local book

The local book stores one symbol's latest reported snapshot in fixed-size,
contiguous quantity arrays. Prices map directly to array slots, and cached
best-price slots make top-of-book reads O(1). Snapshot updates perform bounded
work without heap allocation, tree traversal, or hash lookup inside the book.
The cache goal is to minimize memory traffic and pointer chasing; **zero cache
misses cannot be guaranteed**, even with this layout.

This note describes [LocalBook](../hft/cpp_client/include/local_book.hpp), its
[snapshot message](../hft/cpp_client/include/messages.hpp), and its use in
[the bot](../hft/cpp_client/src/main.cpp). It does not change their behavior.

## Data contract and ownership

Each `BookSnapshotMsg` supplies up to 64 aggregated price levels per side.
Every accepted snapshot replaces the previous reported depth. A missing price
in the next snapshot is removed locally; this is not an incremental add/cancel
protocol. Unreported exchange depth is not reconstructed.

The receive thread owns each book and runs its updates and strategy reads
serially. There are no locks or atomics inside the book. Concurrent readers
would require a separate publication mechanism; setting the validity flag is
not synchronization.

The current bot supports 64 symbol states, allocated together during bot
construction. Each state's optional book is constructed in place on first use.
This avoids a per-book heap allocation, but first-use initialization still
zeros the arrays and can incur cold-cache and page-fault costs. The book accepts
a per-instance tick size; the current bot supplies 0.01 for every symbol.

## Symbol addition and capacity

The exchange's total symbol count is not established by this repository. The
64-symbol limit is a client storage limit, not an exchange guarantee. Symbols
can be registered as they arrive, including previously unseen listings.

The [symbol registry](../hft/cpp_client/include/symbol_registry.hpp) contains
128 fixed hash-table slots. Each occupied slot owns a symbol name of at most
16 bytes, its length, and a stable numeric state ID. Hash collisions use linear
probing. The table refuses new entries beyond its approximately 70% occupancy
limit; the bot's 64-state limit is reached first during normal use.

For an existing symbol, `state_for()` hashes its name, probes for a match, and
returns a pointer into the preallocated state array. It does not reconstruct
the book. For a new symbol, it performs these steps:

1. Reject an empty name or a name longer than 16 bytes.
2. Confirm that the symbol is absent and an unused state remains.
3. Select `states[state_count]` and construct its optional book in place with
   the configured tick size. This initializes the quantity arrays and metadata.
4. Insert the owned symbol name and state ID into the registry. If insertion
   fails, reset the optional book and tick size and increment the failure
   counter without advancing the state count.
5. Copy the symbol name into the state, record its length, and increment
   `state_count`. The receive thread completes registration before returning
   the state to its caller.
6. For a snapshot-triggered registration, apply that snapshot to the new book.

```text
Incoming symbol       Fixed registry          Preallocated states
"AAPL" ----------->  "AAPL" -> ID 0 --------> states[0].local_book
"MSFT" ----------->  "MSFT" -> ID 1 --------> states[1].local_book
new "XYZ" -------->  insert -> ID 2 --------> states[2].local_book.emplace(...)
```

IDs above illustrate arrival order. A state can also be created by an own-fill
callback, so registration need not wait for the first snapshot. A snapshot
whose decoder sets `depth_valid=false` does not register an unseen symbol.
However, a decoded snapshot that later fails book-level validation, such as an
off-grid price, can already have registered its symbol; that state remains.

The 65th distinct symbol cannot obtain a state. The bot increments
`symbol_registration_failures_` and returns no state; a snapshot for that symbol
does not reach the strategy. Existing symbols continue to work. Own fills for
an unregistrable symbol also cannot update a per-symbol position in this
implementation. Capacity therefore needs to cover every symbol the bot may
trade, not just the symbols currently producing snapshots. The counter is not
currently surfaced by a dedicated diagnostic in this path.

### Addition cost

Preallocation removes array growth, rehashing, and per-book heap allocation,
but does not make symbol admission costless. Constructing the book initializes
roughly 17 KiB of arrays on a platform with 8-byte `size_t`, plus metadata.
First-touch memory access can incur cache misses or page faults. Hashing and
copying the name cost O(name length), bounded by 16 bytes; probing depends on
collisions and is bounded by the 128-slot table. Expected lookup cost at this
occupancy is small, but a one-probe lookup is not guaranteed.

Book initialization is O(number of price slots + snapshot capacity); afterward,
normal updates reuse that storage. If the symbol universe is known before
trading, a future warm-up phase could register and touch those books before
timing the strategy. That is an optional optimization, not current startup
behavior. Unknown symbols still require a first-use path. Measure admission
latency separately from established-symbol update latency.

## Symbol deletion: current behavior and possible extension

**There is currently no symbol deletion or slot reuse.** The registry has no
erase operation, `state_count` only increases, and symbol IDs remain stable for
the bot's lifetime. An empty book, a period without updates, a connection drop,
or a session close does not remove the symbol. The session-close handler clears
queue tracking and attempts to flatten tracked positions; it does not reset
the symbol registry or book states. Restarting the process constructs fresh
local state, but is not a substitute for reconciling exchange positions.

This policy avoids deletion work and stale ID reuse, at the cost of retaining
storage for inactive symbols. The limit is **64 distinct admitted symbols over
the process lifetime**, not 64 concurrently active symbols. A symbol that stops
sending snapshots also retains its last book status; silence alone does not
invalidate it or establish that the instrument was delisted.

An empty snapshot means empty reported depth, not deletion. Likewise, clearing
one price level during replacement is unrelated to deleting a symbol.


## Layout and indexing

| State | Purpose | Storage |
| --- | --- | --- |
| Bid and ask quantities | Aggregate shares at each price slot | Two arrays of 1,024 `int64_t` values |
| Base tick | Absolute price tick represented by slot zero | One integer |
| Cached best slots | Highest occupied bid and lowest occupied ask | Two integers; -1 means empty |
| Old occupied slots | Entries to clear on the next replacement | Two arrays of 64 `size_t` values plus counts |
| Status and counters | Validity, recenter events, dropped levels | Fixed-size metadata |

The quantity arrays occupy 16 KiB per book. On a platform with 8-byte `size_t`,
the old-slot lists add 1 KiB. Metadata and padding are additional; measure
`sizeof(LocalBook)` on the target platform. Temporary tick arrays in an update
use another 1 KiB of stack storage. Multiple active symbols increase the working
set, so even a compact individual book does not guarantee L1 residency.

For a valid price:

```text
tick  = round(price / tick_size)
slot  = tick - base_tick
price = (slot + base_tick) * tick_size
```

Conversion rejects nonfinite, nonpositive, out-of-range, and off-grid prices.
Grid tolerance is 1e-6 ticks, and the largest accepted scaled price is
4,503,599,627,370,495 ticks. The grid has origin zero. All array accesses require
`0 <= slot < 1024`; absolute dollar prices are never used as raw indexes.

## Snapshot update

1. Mark the book invalid while validating the replacement. Reject an invalid
   depth flag or counts exceeding the fixed arrays.
2. Convert every reported price to integer ticks and reject nonpositive
   quantities. Find the maximum bid tick and minimum ask tick without assuming
   that input levels are sorted. Even levels that will later be outside the
   band are validated.
3. Choose the band. Retain the current base if both available best prices fit.
   Otherwise recenter around the best prices. Reject a distance of 1,024 ticks
   or more between them because both cannot fit.
4. Clear the old occupied slots **before changing the base**. This removes
   stale quantities without clearing or shifting the entire price band.
5. Store each in-band level at its direct index. Duplicate entries at the same
   tick add together within this snapshot. Track the occupied slots and update
   the best-slot caches while writing quantities. Skip and count distant
   out-of-band levels.
6. Publish the new counts and mark the book valid after replacement completes.

Let `L_old` be the number of retained entries in the previous snapshot and
`L_new` the number of reported entries in the replacement. Update work is
O(`L_old + L_new`), with each count bounded by 128 across both sides. Duplicate
prices may appear more than once in the old-slot list; repeated clearing is
harmless. There is no sort, whole-band shift, or full-band search.

If validation fails, old quantities remain physically stored but accessors
return zero because the book is invalid. The old-slot lists remain available
for cleanup by the next successful update. This prevents stale data from being
presented as the latest valid book. The bot suppresses its strategy callback
when the depth update fails.

## Band placement and boundary invariant

For the available best ticks `low` and `high`, an accepted nonempty snapshot
must satisfy:

```text
0 <= base
high - 1023 <= base <= low
```

The implementation first computes a centered candidate and clamps it to zero.
It then raises that candidate to at least `high - 1023`. The width check ensures
this adjustment does not move the base above `low`.

For example, bid 100.00 and ask 110.23 on a 0.01 grid have ticks 10,000 and
11,023. Naive midpoint rounding produces base 9,999 and puts the ask at invalid
slot 1,024. The corrected base is 10,000: the bid occupies slot 0 and the ask
slot 1,023. Both best prices are retained.

A one-sided book is centered around that side's best price when necessary.
An empty snapshot clears both sides without requiring a band move. Crossed or
locked input is not rejected by this class; the current strategy separately
declines to trade when ask is less than or equal to bid.

### Worked recentering example

Assume a 0.01 tick and 1,024 slots, numbered 0 through 1,023. The endpoint
distance is therefore 1,023 ticks, or $10.23. This is a movable band, not a
maximum stock price.

| Snapshot | Best bid / ask | Base tick | Represented range | Best slots | Action |
| --- | --- | --- | --- | --- | --- |
| First | 100.00 / 100.02 | 9489 | 94.89–105.12 | 511 / 513 | Initial placement |
| Second | 105.10 / 105.12 | 9489 | 94.89–105.12 | 1021 / 1023 | Keep band |
| Third | 105.11 / 105.13 | 10000 | 100.00–110.23 | 511 / 513 | Recenter |

For the first snapshot, the midpoint tick is 10,001 and the base is
`10001 - 512 = 9489`. The second snapshot still fits, including its ask at the
last valid slot. Being near an edge does not by itself trigger recentering.

For the third snapshot, the ask would map to `10513 - 9489 = 1024`, which is
outside the old band. The new midpoint is 10,512 and the new base is
`10512 - 512 = 10000`. Both best prices now sit near the middle:

```text
Old band: 94.89 [---------------------------- bid 105.11 -- 105.12] ask 105.13
                                                                         outside
New band:       100.00 [---- bid 105.11 -- ask 105.13 ----------------] 110.23
```

The update clears entries from the second snapshot using their old indexes,
changes the base, and writes the third snapshot using new indexes. It does not
carry quantities forward, resize the arrays, or move all 1,024 slots. Initial
placement does not increment `recenter_count`; the third snapshot increments
it from zero to one.

The same process works for downward moves. A jump from 100.00/100.02 to
150.00/150.02 can be accepted because both new best prices fit together. In
contrast, 100.00/110.24 is rejected because its 1,024-tick spread cannot fit.
Rejected updates hide old data and prevent the strategy callback until a valid
replacement arrives. A distant non-best depth level alone does not cause
recentering; it is skipped and counted if it falls outside the retained band.

### Recenter cost and timing

The extra band-placement arithmetic and metadata update are O(1). The complete
snapshot update remains O(`L_old + L_new`), whether it recenters or not:

| Work | Maximum entries per update |
| --- | --- |
| Validate new prices/quantities and calculate ticks | 128 across both sides |
| Clear old retained entries | 128 across both sides |
| Visit new levels to write quantities and cache extrema | 128 across both sides |
| Calculate and assign a new base | Constant work |
| Read top-of-book afterward | At most two quantity loads |

Normal snapshot replacement already clears old entries and writes new ones;
recentring adds no full-array pass. Cache behavior can still change because new
indexes may touch different lines. There is no verified isolated nanosecond
measurement for recentering in this note. Earlier live tick-to-order samples
included decoding, strategy execution, and order sending and cannot isolate it.


## Constant-time top-of-book

`top()` checks validity, reads the cached best slots, converts at most two slots
back to prices, and loads at most two quantities. Its work is independent of
depth, gaps between levels, and band width. Empty sides return price and quantity
zero. The quantity-at-price accessors also perform constant work.

The best-slot caches are rebuilt during every replacement, so removing the old
best price never requires a later scan in `top()`. O(1) describes operation
count; cache misses and scheduling can still change elapsed time.

## Keeping memory access local

The implemented layout reduces avoidable cache pressure through:

- Contiguous arrays and arithmetic indexing instead of separately allocated
  price-level nodes and dependent pointer loads.
- Sequential traversal of input levels, temporary ticks, and old-slot lists.
- Clearing only previously touched entries rather than writing all 2,048
  quantity slots on each update.
- Fixed storage, with no allocations, logging, or synchronization in `update()`.
- Immediate strategy reads on the same thread after updating the book, which
  can reuse recently touched data.

Quantity writes and old-slot clears are still indexed accesses and can touch
widely separated cache lines. With a 64-byte cache line, eight adjacent
quantities share a line, but scattered prices need not share one. Cold starts,
symbol rotation, eviction, CPU migration, and other work on the receive path
can all cause misses. The surrounding transport and strategy path has its own
allocations, locks, and logging; the book's properties do not apply to that
entire path.



