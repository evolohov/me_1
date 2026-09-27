// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// matching_engine.hpp
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace me_1 {

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Primitive types

// order ids are supplied by the client. uint64 is cheap to hash/compare and big enough.
using orderId_t = std::uint64_t;

// account ids are strings for just for this test harness. In a real life they would be integers or interned strings.
using accountId_t = std::string;

// proces are integers, obviously
using price_t = std::int64_t;

// signed. no overlap: negative quantity gets rejected right away
using quantity_t = std::int64_t;

// engine assigned sequence numbers, strictly increasing, deterministic. Used for time priority and to detect stale heap entries.
using seqnum_t = std::uint64_t;

// Engine-assigned trade ids, strictly increasing, deterministic.
using tradeId_t = std::uint64_t;

enum class order_side : std::uint8_t { buy, sell };
enum class order_type : std::uint8_t { limit, market };

enum class time_in_force : std::uint8_t 
{ 
    GTC, // rests on the book until filled or cancelled.
    IOC  // match what you can right now, cancel the rest; never rests.
};

// new_/partially_filled are the only states in which an order can be resting on the book; other ones are terminal.
enum class current_order_status : std::uint8_t { new_, partially_filled, filled, cancelled };

// Self-trade prevention (STP) policy
enum class stp_policy : std::uint8_t { cancel_incoming, cancel_resting, cancel_both };

// -----------------------------------------------------------------------------
// Commands (inputs). A std::variant rather than a class hierarchy: it is a
// closed set, value-semantic (copyable into logs), and std::visit makes the
// compiler warn us when we forget to handle a new alternative.
// -----------------------------------------------------------------------------

struct new_order 
{
    orderId_t     id = 0;
    accountId_t   account;
    order_side    side = order_side::buy;
    order_type    type = order_type::limit;
    price_t       price = 0;  // must be 0 for Market, > 0 for Limit
    quantity_t    qty = 0;
    time_in_force tif = time_in_force::GTC;
};

struct cancel_order 
{ 
    orderId_t id = 0;
};

struct stp_policy_set 
{
    stp_policy policy = stp_policy::cancel_incoming;
};

using command = std::variant<new_order, cancel_order, stp_policy_set>;

// -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Order record stored by the engine.
// keep EVERY accepted order forever (including filled / cancelled ones):
//   + status queries work for terminal orders ("was my IOC filled?").
//   + duplicate order-id detection covers ids that are no longer on the book.
//   - memory grows with the session. A production system would archive terminal orders at end of day or after a TTL; out of scope here.
struct order 
{
    orderId_t            id = 0;
    accountId_t          account;
    order_side           side = order_side::buy;
    order_type           type = order_type::limit;
    time_in_force        tif = time_in_force::GTC;
    price_t              price = 0;
    quantity_t           original_qty = 0;
    quantity_t           filled_qty = 0;                      // executed so far
    quantity_t           leaves_qty = 0;                      // still open
    quantity_t           cancelled_qty = 0;                   // removed by cancel/IOC/market remainder/STP
    seqnum_t             seq = 0;                             // time priority
    current_order_status status = current_order_status::new_;    
};

// -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Events (outputs). The engine returns events and lets the caller decide how to publish them. Keeping the event list ordered is part of the determinism contract. 
// events appear in exactly the order the engine performed the corresponding actions.
enum class reject_reason : std::uint8_t 
{
    duplicate_order,      // id already used in this session (even if now terminal)
    bad_quantity,         // qty <= 0
    bad_price,            // limit price <= 0, or market order with price != 0
    bad_account,          // SIP impossible without an account
};

enum class cancel_reject_reason : std::uint8_t 
{
    bad_order,            // never seen this id
    too_late,             // "too late to cancel"
    already_cancelled     // canceled already, or killed by IOC/STP/...
};

enum class cancel_reason : std::uint8_t 
{
    users_request,        // explicit CANCEL command
    ioc_leftover,         // IOC leftover after matching
    market_leftover,      // market order leftover (markets never rest)
    SelfTradePrevention,  // killed by the STP policy
};

struct order_accepted { orderId_t id; seqnum_t seq; };
struct order_rejected { orderId_t id; reject_reason reason; };
struct order_rested   { orderId_t id; order_side side; price_t price; quantity_t qty; };

// One event per maker/taker pair execution. 
// fields are the values AFTER this execution, so a consumer can track order state from the event stream alone without re-querying.
struct trade_event 
{
    tradeId_t   trade_id;
    price_t     price;  // always the RESTING (maker) order's price
    quantity_t  qty;
    orderId_t   taker_id;
    accountId_t taker_account;
    order_side  taker_side;
    quantity_t  taker_leaves;
    orderId_t   maker_id;
    accountId_t maker_account;
    quantity_t  maker_leaves;
};

struct order_cancelled    { orderId_t id; quantity_t cancelled_qty; cancel_reason reason; };
struct cancel_rejected    { orderId_t id; cancel_reject_reason reason; };
struct stp_policy_changed { stp_policy policy; };

using event_t = std::variant<order_accepted, order_rejected, order_rested, trade_event, order_cancelled, cancel_rejected, stp_policy_changed>;

// -----------------------------------------------------------------------------
// Heap entries and priority comparators
// -----------------------------------------------------------------------------
//
// The heap stores small POD entries, NOT whole orders. Reasons:
//   - Heap operations move elements around; moving 24 or so bytes is much cheaper than moving a struct containing a std::string.
//   - The authoritative mutable order state (leaves, status) lives in a single place-the orders map, so a partial fill updates the map and the heap entry does not
//     need to change at all -> the order keeps its position,so partial fills remain in their place
struct heap_entry { price_t price; seqnum_t seq; orderId_t id; };

// bids. max-heap on price (highest bid is best), when ties then lower seq is better.
struct priority_bid 
{
    bool operator()(const heap_entry& a, const heap_entry& b) const 
    {
        return a.price != b.price?  a.price < b.price : a.seq > b.seq;
    }
};

// asks: min-heap on price (lowest offer is best),when ties -> lower seq is better.
struct priority_ask 
{
    bool operator()(const heap_entry& a, const heap_entry& b) const 
    {
        return a.price != b.price? a.price > b.price : a.seq > b.seq;
    }
};

// -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// side_heap: one side of the book as a binary heap over a std::vector.
//
//  raw std::vector + std::*_heap instead of std::priority_queue?
//  priority_queue does no allow container access which is necessary for order-level depth snapshots and  compaction
//  of cancelled entries (see below). A vector also copies cheaply
//
// cancelation is actually lazy deleteion (converting to tombstones).
//   A binary heap cannot remove an arbitrary element in O(log n) without an auxiliary position index that must be updated every time. Instead to cancel just flips the order's 
//   set status in the orders map; its heap entry becomes a dead "tombstone" that is discarded whenever it surfaces at the top.
//   Cost: cancel is O(1) (+ O(log L) level bookkeeping); the problem is tghat dead entries occupy memory until popped. to regain that memory compaction is used once tombstones 
//   make up at least half of the heap and exceed a small number.
//
// Alternatives considered:
// -- std::map<Price, std::deque<Order>> (the classic "levels + FIFO" design):
//     O(log L) best-price access, O(1) iteration of depth, easy cancel with an iterator index. Arguably better for production, but I wanted to play with heaps. and heaps have 
//     excellent cache behaviour for push/pop.
// -- indexed heap (track each entry's position): true O(log n) removal, at the cost of bookkeeping on every sneeze. Do not think its worth it.
template <class Priority> struct side_heap 
{
    void push(const heap_entry& e) 
    {
        heap_.push_back(e);
        std::push_heap(heap_.begin(), heap_.end(), Priority{});
    }

    // remove the top because the order is filled or STP-cancelled (live entry).
    void pop(void) 
    {
        std::pop_heap(heap_.begin(), heap_.end(), Priority{});
        heap_.pop_back();
    }
    
    // remove the top because it is a tombstone (order was cancelled earlier).
    void pop_dead(void) 
    {
        pop();
        if(tombstones_ > 0) --tombstones_;
    }

    bool empty(void)                const { return heap_.empty(); }
    std::size_t size(void)          const { return heap_.size();  }  
    const heap_entry& top(void)     const { return heap_.front(); }
    void add_tombstone(void)              { ++tombstones_;        }
    std::size_t tombstones(void)    const { return tombstones_;   }
    
    // rebuild the heap keeping only entries for which is_live(entry) is true.
    // std::remove_if preserves relative order and std::make_heap is a pure function of its input, so compaction is deterministic too 
    template <class IsLive> void compact(IsLive is_live) 
    {
        heap_.erase(std::remove_if(heap_.begin(), heap_.end(), [&](const heap_entry& e) { return !is_live(e); }), heap_.end());
        std::make_heap(heap_.begin(), heap_.end(), Priority{});
        this->tombstones_ = 0;
    }

    // read-only access for snapshots and invariant checks. NOT sorted.
    const std::vector<heap_entry>& entries() const { return heap_; }

 private:
    std::vector<heap_entry> heap_;
    std::size_t   tombstones_ = 0;  // dead entries still physically in heap    
};

// small side index price -> <total open qty, number of orders>
// updated when an order rests, when a resting order is filled or partially filled, and when a resting order is cancelled.
// a little extra work per event and a second structure that must be kept in sync but we getting  O(1) top-of-book and O(L) depth. 
// std::map is used because iteration order is sorted and deterministic
struct level_agg { quantity_t qty = 0; std::uint32_t order_count = 0; };
using levels_t = std::map<price_t, level_agg>;

// -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// single instrument.
struct book_state {
    std::string symbol = "XYZ";
    stp_policy stp = stp_policy::cancel_incoming;

    side_heap<priority_bid> bids;  // max-heap by price
    side_heap<priority_ask> asks;  // min-heap by price

    levels_t bid_levels;
    levels_t ask_levels;

    // unordered_map for O(1) lookup on cancel/status/match.    
    std::unordered_map<orderId_t, order> orders;

    seqnum_t next_seq = 1;
    tradeId_t next_trade_id = 1;
};

// me_1: exactly one book. 
// me_2: std::map<symbol, book_state> + id index.
struct engine_state { book_state book; };
engine_state make_engine(std::string symbol = "xyz", stp_policy stp = stp_policy::cancel_incoming);

// -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// API
struct step_result { engine_state state;  std::vector<event_t> events; };

// Apply oommand. Pure: the result depends only on (state, cmd).
step_result apply(engine_state state, const command& cmd);

// Left fold of apply() over a command log. Events of all commands are concatenated in order. 
step_result replay(engine_state initial, const std::vector<command>& commands);


// -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// queries. do not modify any state.
struct level_view { price_t price; quantity_t qty; std::uint32_t order_count; };

struct top_of_book_view 
{
    std::optional<level_view> bid;
    std::optional<level_view> ask;

    // the order that would match first on each side (front of the queue at the best price). Read straight from the heap top;
    std::optional<orderId_t> first_bid_id;
    std::optional<orderId_t> first_ask_id;
};

struct book_depth_view
{
    std::vector<level_view> bids;  // best (highest) first
    std::vector<level_view> asks;  // best (lowest) first
};

struct resting_order_view 
{
    orderId_t   id;
    accountId_t account;
    price_t     price;
    quantity_t  leaves;
    seqnum_t    seq;
};

struct order_depth_view 
{
  std::vector<resting_order_view> bids;  // exact matching priority order
  std::vector<resting_order_view> asks;
};

top_of_book_view top_of_book(const engine_state& s);                        // O(1)
book_depth_view depth(const engine_state& s, std::size_t max_levels = 0);   // 0 = all
order_depth_view order_depth(const engine_state& s);                        // O(n log n)
std::optional<order> order_status(const engine_state& s, orderId_t id);     // O(1)

// consistency check (heap validity, level aggregates vs heap contents, etc
// Returns nullopt if everything is consistent.
std::optional<std::string> check_consistency(const engine_state& s);

// Stable string names (used by the CLI and golden test files).
const char* to_string(order_side);
const char* to_string(order_type);
const char* to_string(time_in_force);
const char* to_string(current_order_status);
const char* to_string(stp_policy);
const char* to_string(reject_reason);
const char* to_string(cancel_reject_reason);
const char* to_string(cancel_reason);

}  // namespace me_1