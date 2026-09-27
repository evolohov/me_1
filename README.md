# me_1
A single-instrument continuous limit order book with price-time priority.

The engine is a *pure state-transition function*:
`(engine_state, Command)  ->  (engine_state', [event_t...])`

Everything the engine knows lives in engine_state. There are no globals, no clocks, no random numbers, no I/O and no threads inside the engine. That is
what makes it deterministic *by construction* rather than by testing alone: if the function only depends on its inputs,
the same inputs must yield the same outputs. The CLI, file replay, journal  persistence, etc. are all thin shells around that function.
Use min heap for `sell` order book and max heap for `buy` order book. Standard approach with `std::map<Price, std::deque<Order>>` would be more suitable
for production, but I want to see what can be done with heaps and going to use it later 

* Determinism is trivial to reason about: state in, state out.
* Replay/event-sourcing is a left fold over the input log.
* Testing needs no mocks: feed commands, compare the event list.
* Tradeoff: a naive pure function would copy the whole book on every call. avoiding that by taking the state *by value* so callers can std::move it
   in (O(1), no copy) -- see apply() sporce code. 


## Key decisions and tradeoffs (details are in the source comments)

**Heaps with lazy deletion.** A binary heap cannot remove an arbitrary element cheaply, so cancel marks the order dead and its heap entry becomes a tombstone that is discarded when it reaches the top. Cancel becomes O(1). When tombstones are at least half the heap (and at least 64), the heap is rebuilt in O(n), which amortises to O(1) per cancel. The classic alternative, `std::map<Price, deque>`, has cheaper depth queries and direct cancels, and would be a reasonable production choice; 

**Heap entries are small and immutable.** They hold only price, sequence number and order id. Mutable order state lives in one hash map. A partial fill therefore changes nothing in the heap, which is exactly why a partially filled order keeps its priority.

**Aggregated price levels on the side.** A `std::map<Price, {qty, count}>` per side gives O(1) top-of-book and O(levels) depth without sorting the heap. The cost is a second structure to keep consistent; `check_consistency()` recomputes it from the heaps and compares.

**Self-trade prevention is configurable and part of the input stream.** The default, cancel-incoming, leaves the resting order's queue position intact. 

**Market orders never rest,** whatever their TIF, because a resting order needs a price to be ranked. There is no price collar, so a market order can sweep the whole book; 

**"Cancel during match" is resolved by sequencing, not locking.** The engine is single-threaded and each command is atomic, so a cancel is always applied entirely before or entirely after a match. 

**All orders are kept forever** so status queries and duplicate-id checks work for finished orders. Memory grows with the session; production would archive terminal orders.


## Using it

```
./me replay tests/01_price_time_priority   # print every event
./me verify tests/05_cancel_edge_cases     # run twice, compare, check invariants
./me repl                                  # interactive
./me exec --journal book.log "PLACE 1 A BUY LIMIT 100 10 GTC" "TOP"
./me exec --journal book.log "CANCEL 1" "STATUS 1"   # state persists via the journal
```

Input language (keywords case-insensitive, `#` comments, prices are integer ticks).

```
PLACE <id> <account> <BUY|SELL> <LIMIT|MARKET> <price> <qty> [GTC|IOC]
CANCEL <id>
CONFIG STP <CANCEL_INCOMING|CANCEL_RESTING|CANCEL_BOTH>
TOP | DEPTH [ORDERS] | STATUS <id>
```
