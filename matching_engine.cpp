#include "matching_engine.hpp"
#include <sstream>

namespace me_1 {

namespace { // internal

// Standard helpers for std::visit with lambdas.
template <class... Ts> struct overloaded : Ts... {   using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// Compacting a tiny heap is pointless; 64 dead entries of 24 bytes is like ~2 kb. 
constexpr std::size_t kMinTombstonesForCompaction = 64;

bool is_resting_status(current_order_status s) 
{
    return s == current_order_status::new_ || s == current_order_status::partially_filled;
}

// A heap entry is live if its order still exists, is still resting, and the  entry belongs to the order's current incarnation (seq matches).
// The seq check is redundant today (ids are never reused), but it is can be used to "amend/replace" that loses priority. 
// re-pushing with a new seq makes old entry a tombstone.
bool is_live(const book_state& b, const heap_entry& e) 
{
    auto it = b.orders.find(e.id);

    return it != b.orders.end() && it->second.seq == e.seq && is_resting_status(it->second.status);
}

// -- price level book-keeping ---------------------------------------------------------------------------------------------------------------------------------------------------
void level_add(levels_t& lv, price_t px, quantity_t q) 
{
    level_agg& l = lv[px];  // creates the level if missing
    l.qty += q;
    l.order_count += 1;
}

// reduce a level by q; if `order_removed` the order leaves the level entirely, empty levels_t are erased so that begin()/rbegin() is always the best price.
void level_reduce(levels_t& lv, price_t px, quantity_t q, bool order_removed) 
{
    auto it = lv.find(px);
    if(it == lv.end()) return;  // cannot happen; check_consistency would flag it

    it->second.qty -= q;

    if(order_removed) 
      it->second.order_count -= 1;

    if(it->second.order_count == 0) 
      lv.erase(it);
}

// -- heap maintenance -----------------------------------------------------------------------------------------------------------------------------------------------------------

// Discard dead ones at the top.
template <class P> void clean_top(const book_state& b, side_heap<P>& h) 
{
    while(!h.empty() && !is_live(b, h.top())) 
      h.pop_dead();
}

template <class P> void maybe_compact(const book_state& b, side_heap<P>& h) 
{
    if(h.tombstones() >= kMinTombstonesForCompaction && h.tombstones() * 2 >= h.size()) 
    {
        h.compact([&](const heap_entry& e) { return is_live(b, e); });
    }
}

// -- matching helpers -----------------------------------------------------------------------------------------------------------------------------------------------------------

// market's price?
bool crosses(const order& taker, price_t maker_px) 
{
    if(taker.type == order_type::market) 
      return true;

    return taker.side == order_side::buy ? maker_px <= taker.price : 
                                           maker_px >  taker.price;
}

// kill the remaining qty of the INCOMING order (it is not on the book, so no heap / level bookkeeping is needed)
void cancel_incoming_remainder(order& o, cancel_reason why, std::vector<event_t>& ev) 
{
    quantity_t q = o.leaves_qty;
  
    o.cancelled_qty += q;
    o.leaves_qty = 0;
    o.status = current_order_status::cancelled;
  
    ev.push_back(order_cancelled{o.id, q, why});
}

// kill a resting order that is currently at the top of its heap (STP case).
template <class P> void cancel_resting_top(side_heap<P>& h, levels_t& lv, order& maker, cancel_reason why, std::vector<event_t>& ev) 
{
  
    quantity_t q = maker.leaves_qty;
  
    level_reduce(lv, maker.price, q, /*order_removed=*/true);

    maker.cancelled_qty += q;
    maker.leaves_qty = 0;
    maker.status = current_order_status::cancelled;

    h.pop();
    ev.push_back(order_cancelled{maker.id, q, why});
}

// Core sweep of an incoming order against the opposite side.
// Returns true if the incoming order was terminated by self-trade prevention the caller must not rest
//
// Price-time priority is enforced entirely by the heap: trade happens only with opp.top(), which is by construction the best price and within that
// price is the earliest seq.
template <class P> bool sweep(book_state& b, order& taker, side_heap<P>& opp, levels_t& opp_levels, std::vector<event_t>& ev) 
{
    while (taker.leaves_qty > 0) 
    {
        clean_top(b, opp);  // skip orders cancelled earlier

        if (opp.empty())    // nothing to do
          break;

        // copy. the entry is about to be removed.
        const heap_entry top = opp.top();

        if(!crosses(taker, top.price)) 
            break;              // best price no longer marketable

        // .at(): a live heap entry without an order would be an engine bug; throwing is better than undefined behaviour.
        order& maker = b.orders.at(top.id);

        // --- self-trade prevention ------------------------------------------------
        // Checked only for a would-be trade (i.e. after the price check): an own order that the taker would not reach anyway is not a self-trade.
        // it could/should be written as a separate function and could be called along with other validation functions to prevent illigal trades
        // like company A cannot trade with company B, etc... may be some rules engine.
        if (maker.account == taker.account) 
        {
            switch (b.stp) 
            {
                case stp_policy::cancel_incoming:
                    cancel_incoming_remainder(taker, cancel_reason::SelfTradePrevention, ev);
                    return true;
                case stp_policy::cancel_resting:
                    cancel_resting_top(opp, opp_levels, maker, cancel_reason::SelfTradePrevention, ev);
                    continue;  // check next best order
                case stp_policy::cancel_both:
                    // event order: resting first, then incoming. Arbitrary but FIXED which is all determinism requires
                    cancel_resting_top(opp, opp_levels, maker, cancel_reason::SelfTradePrevention, ev);
                    cancel_incoming_remainder(taker, cancel_reason::SelfTradePrevention, ev);
                    return true;
            }
            // done here
        }

        // --- execution ------------------------------------------------------------
        const quantity_t q = std::min(taker.leaves_qty, maker.leaves_qty);
        // Trade at the MAKER's price: the resting order set the price; the taker gets any price improvement. Universal convention for continuous books.
        const price_t px = maker.price;

        taker.leaves_qty -= q;
        taker.filled_qty += q;
        maker.leaves_qty -= q;
        maker.filled_qty += q;

        const bool maker_done = (maker.leaves_qty == 0);
        level_reduce(opp_levels, maker.price, q, maker_done);

        if(maker_done)
        {
            maker.status = current_order_status::filled;
            opp.pop();  // fully filled makers        
        } else {
            // partial fill keep original priority
            maker.status = current_order_status::partially_filled;
        }

        taker.status = taker.leaves_qty == 0 ? current_order_status::filled
                                             : current_order_status::partially_filled;

        ev.push_back(trade_event{b.next_trade_id++, px, q, taker.id, taker.account, taker.side, taker.leaves_qty, maker.id, maker.account, maker.leaves_qty});
    }

    return false;
}

void rest_order(book_state& b, const order& o, std::vector<event_t>& ev) 
{
    const heap_entry e{o.price, o.seq, o.id};

    if(o.side == order_side::buy) {

      b.bids.push(e);
      level_add(b.bid_levels, o.price, o.leaves_qty);

    } else {

      b.asks.push(e);
      level_add(b.ask_levels, o.price, o.leaves_qty);

    }

    ev.push_back(order_rested{o.id, o.side, o.price, o.leaves_qty});
}

// Validation happens before any state change, so a rejected order leaves the  book bit-for-bit untouched and consumes no sequence number. (Consuming a
// seq for rejects would also be deterministic; not consuming keeps seq dense
// over accepted orders, which is nicer to read in tests.)
// The check order fixes which reason is reported when several apply.
std::optional<reject_reason> validate(const book_state& b, const new_order& n) 
{
    if(b.orders.count(n.id) != 0)                    return reject_reason::duplicate_order;
    if(n.account.empty())                            return reject_reason::bad_account;
    if(n.qty <= 0)                                   return reject_reason::bad_quantity;
    if(n.type == order_type::limit && n.price <= 0)  return reject_reason::bad_price;
    // price should be 0 for market orders
    if(n.type == order_type::market && n.price != 0) return reject_reason::bad_price;

    // everything is just fine
    return std::nullopt;
}

void handle_new(book_state& b, const new_order& n, std::vector<event_t>& ev) 
{
    if(auto why = validate(b, n)) 
    {
      ev.push_back(order_rejected{n.id, *why});    

    } else {

        order o;
        o.id           = n.id;
        o.account      = n.account;
        o.side         = n.side;
        o.type         = n.type;
        o.tif          = n.tif;
        o.price        = n.price;
        o.original_qty = n.qty;
        o.leaves_qty   = n.qty;
        o.seq          = b.next_seq++;  // time priority is fixed at acceptance, never changes
        o.status       = current_order_status::new_;

        // insert first so the order is queryable and the taker reference below is stable (unordered_map references survive rehash; nothing else is
        // inserted during the sweep anyway).
        order& taker = b.orders.emplace(o.id, std::move(o)).first->second;
        ev.push_back(order_accepted{taker.id, taker.seq});

        const bool killed_by_stp = taker.side == order_side::buy ? sweep(b, taker, b.asks, b.ask_levels, ev)
                                                                 : sweep(b, taker, b.bids, b.bid_levels, ev);

        if(!killed_by_stp && taker.leaves_qty > 0) 
        {
            if (taker.type == order_type::market) {
              // Market orders never rest, regardless of TIF: a resting order needs a price to be ranked by, and a market order has none. 
              cancel_incoming_remainder(taker, cancel_reason::market_leftover, ev);
            } else if (taker.tif == time_in_force::IOC) {
              cancel_incoming_remainder(taker, cancel_reason::ioc_leftover, ev);
            } else {
              // GTC limit with remaining qty: joins the BACK of the queue at its
              // price (its seq is the newest). Status is New or PartiallyFilled.
              rest_order(b, taker, ev);
            }
        }

        // make sure that heap tops are live
        clean_top(b, b.bids);
        clean_top(b, b.asks);
    }
}

void handle_cancel(book_state& b, const cancel_order& c, std::vector<event_t>& ev) 
{
    auto it = b.orders.find(c.id);
    
    if(it == b.orders.end()) 
    {
        ev.push_back(cancel_rejected{c.id, cancel_reject_reason::bad_order});
        return;
    }
    
    order& o = it->second;
    if(o.status == current_order_status::filled) 
    {
        ev.push_back(cancel_rejected{c.id, cancel_reject_reason::too_late});
        return;
    }

    if(o.status == current_order_status::cancelled) 
    {
        ev.push_back(cancel_rejected{c.id, cancel_reject_reason::already_cancelled});
        return;
    }

    // resting order (only GTC limits can be resting). Cancel just the leaves. any quantity already filled stays filled, obviously.
    const quantity_t q = o.leaves_qty;

    levels_t& lv = o.side == order_side::buy ? b.bid_levels : b.ask_levels;
    level_reduce(lv, o.price, q, /*order_removed=*/true);
    o.cancelled_qty += q;
    o.leaves_qty = 0;
    o.status = current_order_status::cancelled;

    ev.push_back(order_cancelled{o.id, q, cancel_reason::users_request});

    // Lazy deletion: the heap entry stays as a tombstone.
    if(o.side == order_side::buy) 
    {
        b.bids.add_tombstone();
        clean_top(b, b.bids);  // if we cancelled the best bid the get the next one on top
        maybe_compact(b, b.bids);

    } else {
        b.asks.add_tombstone();
        clean_top(b, b.asks);
        maybe_compact(b, b.asks);
    }
}

void step(book_state& b, const command& cmd, std::vector<event_t>& ev) 
{
    std::visit(overloaded{ [&](const new_order& n)      { handle_new(b, n, ev); },
                           [&](const cancel_order& c)   { handle_cancel(b, c, ev); },
                           [&](const stp_policy_set& s) { b.stp = s.policy;
                                                          ev.push_back(stp_policy_changed{s.policy}); }, }, cmd);
}

// order-level snapshot of one side, in exact matching priority.
// Copy the live entries and sort with the SAME comparator the heap uses, so  the printed order is by definition the order in which they would fill.
// std::sort is not stable, but with a strict total order stability is moot.
template <class P> std::vector<resting_order_view> ordered_side(const book_state& b, const side_heap<P>& h) 
{
    std::vector<heap_entry> live;
    live.reserve(h.size());

    for (const heap_entry& e : h.entries()) 
        if (is_live(b, e)) 
            live.push_back(e);

    // P{}(y, x) == "y has lower priority than x" -> x goes first.
    std::sort(live.begin(), live.end(), [](const heap_entry& x, const heap_entry& y) { return P{}(y, x); });
    std::vector<resting_order_view> out;
    out.reserve(live.size());
    for (const heap_entry& e : live) 
    {
        const order& o = b.orders.at(e.id);
        out.push_back(resting_order_view{o.id, o.account, o.price, o.leaves_qty, o.seq});
    }

    return out;
}

template <class P> std::optional<std::string> check_side(const book_state& b, const side_heap<P>& h, const levels_t& levels, order_side side, std::size_t& live_count) {

    const char* name = to_string(side);

    if(!std::is_heap(h.entries().begin(), h.entries().end(), P{}))
        return std::string(name) + ": heap corrupted";

    if(!h.empty() && !is_live(b, h.top()))
      return std::string(name) + ": heap top is dead ";

    levels_t recomputed;
    std::size_t dead = 0;

    for(const heap_entry& e : h.entries()) 
    {
        if(!is_live(b, e)) { ++dead; continue; }

        const order& o = b.orders.at(e.id);

        if(o.side != side)      return std::string(name) + ": order on wrong side";
        if (o.price != e.price) return std::string(name) + ": entry price mismatch";
        if (o.leaves_qty <= 0)  return std::string(name) + ": live order with no leaves";

        level_add(recomputed, o.price, o.leaves_qty);
        live_count += 1;
    }

    if(dead != h.tombstones())             return std::string(name) + ": tombstone count mismatch";
    if(recomputed.size() != levels.size()) return std::string(name) + ": level count mismatch";
    
    for(auto a = recomputed.cbegin(), c = levels.cbegin(); a != recomputed.cend(); ++a, ++c) 
    {
        if(a->first != c->first || a->second.qty != c->second.qty || a->second.order_count != c->second.order_count)
            return std::string(name) + ": level aggregate mismatch at price " + std::to_string(a->first);
    }
  
    return std::nullopt;
  }

}  // anonymous namespace

// =============================================================================
// Public API
// =============================================================================
engine_state make_engine(std::string symbol, stp_policy stp) 
{
    engine_state s;

    s.book.symbol = std::move(symbol);
    s.book.stp = stp;

    return s;
}

step_result apply(engine_state state, const command& cmd) 
{
    step_result r;

    step(state.book, cmd, r.events);
    r.state = std::move(state);

    return r;
}

step_result replay(engine_state initial, const std::vector<command>& commands) 
{
    step_result r;

    for (const command& c : commands) 
        step(initial.book, c, r.events);
    
    r.state = std::move(initial);

    return r;
}

top_of_book_view top_of_book(const engine_state& s) 
{
    const book_state& b = s.book;
    top_of_book_view t;

    if(!b.bid_levels.empty()) 
    {
      const auto& [px, agg] = *b.bid_levels.rbegin();  // highest bid
      t.bid = level_view{px, agg.qty, agg.order_count};
    }

    if(!b.ask_levels.empty()) 
    {
        const auto& [px, agg] = *b.ask_levels.begin();  // lowest ask
        t.ask = level_view{px, agg.qty, agg.order_count};
    }

    if(!b.bids.empty()) t.first_bid_id = b.bids.top().id;
    if(!b.asks.empty()) t.first_ask_id = b.asks.top().id;

    return t;
}

book_depth_view depth(const engine_state& s, std::size_t max_levels) 
{
  const book_state& b = s.book;
  book_depth_view d;

  for(auto it = b.bid_levels.rbegin(); it != b.bid_levels.rend(); ++it) 
  {
    if(max_levels && d.bids.size() >= max_levels) 
      break;

    d.bids.push_back(level_view{it->first, it->second.qty, it->second.order_count});
  }

  for(auto it = b.ask_levels.begin(); it != b.ask_levels.end(); ++it) 
  {
      if(max_levels && d.asks.size() >= max_levels) 
        break;
      
      d.asks.push_back(level_view{it->first, it->second.qty, it->second.order_count});
  }

  return d;
}

order_depth_view order_depth(const engine_state& s) 
{
    return order_depth_view{ordered_side(s.book, s.book.bids), ordered_side(s.book, s.book.asks)};
}

std::optional<order> order_status(const engine_state& s, orderId_t id) 
{
    auto it = s.book.orders.find(id);

    // return by val to prevent mutation
    return it == s.book.orders.end()? std::nullopt:std::optional<order>(it->second);  
}

std::optional<std::string> check_consistency(const engine_state& s) 
{
    const book_state& b = s.book;
    std::size_t live_bids = 0, live_asks = 0;

    if(auto e = check_side(b, b.bids, b.bid_levels, order_side::buy, live_bids))  return e;
    if(auto e = check_side(b, b.asks, b.ask_levels, order_side::sell, live_asks)) return e;

    if(!b.bid_levels.empty() && !b.ask_levels.empty() && b.bid_levels.rbegin()->first >= b.ask_levels.begin()->first)
      return std::string("book is crossed");

    std::size_t resting_bids = 0, resting_asks = 0;
    for(const auto& [id, o] : b.orders) 
    {
        if(o.filled_qty + o.leaves_qty + o.cancelled_qty != o.original_qty)
            return "qty not conserved for order " + std::to_string(id);

        if(is_resting_status(o.status)) 
        {
          if(o.type != order_type::limit || o.tif != time_in_force::GTC)
            return "non-GTC-limit order resting: " + std::to_string(id);
          else   
            (o.side == order_side::buy ? resting_bids : resting_asks) += 1;

        } else if (o.leaves_qty != 0) 
            return "terminal order with leaves: " + std::to_string(id);
    
    }

    // Every resting order appears exactly once in its heap.
    return resting_bids != live_bids || resting_asks != live_asks? std::optional<std::string>("resting orders vs live heap entries mismatch"): std::nullopt;
}

// Stable names. Kept here (not in the CLI) for printed strings consistency
const char* to_string(order_side v)    { return v == order_side::buy    ? "BUY"   : "SELL"; }
const char* to_string(order_type v)    { return v == order_type::limit  ? "LIMIT" : "MARKET"; }
const char* to_string(time_in_force v) { return v == time_in_force::GTC ? "GTC"   : "IOC"; }
const char* to_string(current_order_status v) 
{
    switch (v) 
    {
        case current_order_status::new_:             return "NEW";
        case current_order_status::partially_filled: return "PARTIALLY_FILLED";
        case current_order_status::filled:           return "FILLED";
        case current_order_status::cancelled:        return "CANCELLED";
    }

    return "?";
}
const char* to_string(stp_policy v) 
{
    switch (v) 
    {
        case stp_policy::cancel_incoming: return "CANCEL_INCOMING";
        case stp_policy::cancel_resting:  return "CANCEL_RESTING";
        case stp_policy::cancel_both:     return "CANCEL_BOTH";
    }

  return "?";
}
const char* to_string(reject_reason v) 
{
    switch (v) 
    {
        case reject_reason::duplicate_order: return "DUPLICATE_ORDER_ID";
        case reject_reason::bad_quantity:    return "BAD_QUANTITY";
        case reject_reason::bad_price:       return "BAD_PRICE";
        case reject_reason::bad_account:     return "BAD_ACCOUNT";
    }

    return "?";
}
const char* to_string(cancel_reject_reason v) 
{
    switch (v) 
    {
        case cancel_reject_reason::bad_order:         return "BAD_ORDER";
        case cancel_reject_reason::too_late:          return "TOO_LATE";
        case cancel_reject_reason::already_cancelled: return "ALREADY_CANCELLED";
    }

    return "?";
}
const char* to_string(cancel_reason v) 
{
    switch (v) 
    {
      case cancel_reason::users_request:       return "USER_REQUESTED";
      case cancel_reason::ioc_leftover:        return "IOC_REMAINDER";
      case cancel_reason::market_leftover:     return "MARKET_REMAINDER";
      case cancel_reason::SelfTradePrevention: return "SELF_TRADE_PREVENTION";
    }

  return "?";
}

}  // namespace me_1