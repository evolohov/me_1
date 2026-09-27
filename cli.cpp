// ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// cli.cpp -- thin command-line shell
//
// parse text into Commands/Queries, call me::apply(), render events and query results as text. No matching logic lives here. 
//
// MODES
//   me replay <file>             run a scenario file, print every event
//   me verify <file>             run it twice + invariant checks after every step; compare transcripts byte-for-byte
//   me repl                      interactive, commands from stdin
//   me exec [--journal F] CMD... apply commands given as arguments; with --journal the book persists between runs
//   me gen <seed> <count>        emit a pseudo-random but reproducible command stream (for verify / stress)
//
// INPUT LANGUAGE (one command per line, keywords case-insensitive, '#' starts  a comment):
//   PLACE <id> <account> <BUY|SELL> <LIMIT|MARKET> <price> <qty> [GTC|IOC]
//   CANCEL <id>
//   CONFIG STP <CANCEL_INCOMING|CANCEL_RESTING|CANCEL_BOTH>
//   TOP                  best bid/ask with aggregated size
//   DEPTH [ORDERS]       price-level ladder, or every resting order in
//                        exact matching-priority order
//   STATUS <id>
// =============================================================================
#include "matching_engine.hpp"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace me = me_1;

namespace {

// ---------------------------------------------------------------------------
// Queries are separate from me::Command on purpose: they are read-only, so
// they never go into a journal and never influence determinism.
// ---------------------------------------------------------------------------
struct QueryTop {};
struct QueryDepth { bool orders = false; };
struct QueryStatus { me::orderId_t id = 0; };

using Line = std::variant<me::command, QueryTop, QueryDepth, QueryStatus>;

template <class... Ts> struct overloaded : Ts... 
{

    using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

std::string upper(std::string s) 
{
    for (char& c : s) 
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    return s;
}

std::vector<std::string> tokenize(const std::string& raw) 
{
    const std::string line = raw.substr(0, raw.find('#'));  // strip comment

    std::istringstream in(line);
    std::vector<std::string> out;

    for(std::string t; in >> t;) 
        out.push_back(t);

    return out;
}

// std::from_chars: locale-independent, no exceptions, rejects trailing junk
// (we require the whole token to be consumed). std::stoll would accept "12abc".
template <class T> bool parse_int(const std::string& s, T& out) 
{
    const char* first = s.data();
    const char* last = s.data() + s.size();

    auto [ptr, ec] = std::from_chars(first, last, out);

    return ec == std::errc() && ptr == last;
}

enum class parse_status { blank, ok, error };

parse_status parse_line(const std::string& raw, Line& out, std::string& err) 
{
    const auto tok = tokenize(raw);
    if (tok.empty()) return parse_status::blank;

    const std::string kw = upper(tok[0]);
    auto fail = [&](std::string msg) { err = std::move(msg); return parse_status::error; };

    if(kw == "PLACE") 
    {
        // TIF is optional (defaults to GTC) purely for typing convenience.
        if(tok.size() != 7 && tok.size() != 8)
            return fail("usage: PLACE <id> <account> <BUY|SELL> <LIMIT|MARKET> <price> <qty> [GTC|IOC]");

        me::new_order n;

        if(!parse_int(tok[1], n.id)) 
            return fail("bad order id: " + tok[1]);

        n.account = tok[2];  // accounts are case-SENSITIVE identifiers; not upper()'d
        const std::string side = upper(tok[3]), type = upper(tok[4]);

        if(side == "BUY" || side == "B") 
            n.side = me::order_side::buy;
        else if(side == "SELL" || side == "S") 
            n.side = me::order_side::sell;
        else 
            return fail("bad side: " + tok[3]);

        if (type == "LIMIT" || type == "L") 
            n.type = me::order_type::limit;
        else if (type == "MARKET" || type == "M") 
            n.type = me::order_type::market;        
        else
            return fail("bad order type: " + tok[4]);

        // Syntax errors are the parser's job; semantic errors (price 0 on a limit,
        // qty 0, ...) are left to the ENGINE so they are rejected with a proper
        // event and are identical no matter which front end sent them.
        if(!parse_int(tok[5], n.price)) return fail("bad price: " + tok[5]);
        if(!parse_int(tok[6], n.qty))   return fail("bad qty: " + tok[6]);
        if(tok.size() == 8) 
        {
            std::string tif = upper(tok[7]);

            if (tif == "GTC")      n.tif = me::time_in_force::GTC;
            else if (tif == "IOC") n.tif = me::time_in_force::IOC;
            else return fail("bad time in force: " + tok[7]);
        }

        out = me::command{n};
        return parse_status::ok;
    } // place

    if(kw == "CANCEL") 
    {
        if(tok.size() != 2) return fail("usage: CANCEL <id>");

        me::cancel_order c;
        if (!parse_int(tok[1], c.id)) return fail("bad order id: " + tok[1]);
    
        out = me::command{c};
        return parse_status::ok;
    } // cancel

    if(kw == "CONFIG") 
    {
        if (tok.size() != 3 || upper(tok[1]) != "STP")
            return fail("usage: CONFIG STP <CANCEL_INCOMING|CANCEL_RESTING|CANCEL_BOTH>");
    
        std::string p = upper(tok[2]);

        me::stp_policy_set s;
        if (p == "CANCEL_INCOMING")     s.policy = me::stp_policy::cancel_incoming;
        else if (p == "CANCEL_RESTING") s.policy = me::stp_policy::cancel_resting;
        else if (p == "CANCEL_BOTH")    s.policy = me::stp_policy::cancel_both;
        else return fail("bad STP policy: " + tok[2]);

        out = me::command{s};
        return parse_status::ok;
    } // config

    if(kw == "TOP") 
    {
        if (tok.size() != 1) return fail("usage: TOP");

        out = QueryTop{};

        return parse_status::ok;
    } // top

    if(kw == "DEPTH") 
    {

        if (tok.size() == 1)                              { out = QueryDepth{false}; return parse_status::ok; }
        if (tok.size() == 2 && upper(tok[1]) == "ORDERS") { out = QueryDepth{true};  return parse_status::ok; }
        return fail("usage: DEPTH [ORDERS]");
    } // depth

    if (kw == "STATUS") 
    {
        if (tok.size() != 2) return fail("usage: STATUS <id>");

        QueryStatus q;
        if (!parse_int(tok[1], q.id)) return fail("bad order id: " + tok[1]);

        out = q;

        return parse_status::ok;
    }

    return fail("unknown command: " + tok[0]);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Fixed, human-readable, grep-friendly format. 
std::string render(const me::event_t& e)
{
    std::ostringstream o;

    std::visit(overloaded {
        [&](const me::order_accepted& x)     { o << "ACCEPTED         id=" << x.id << " seq=" << x.seq; },
        [&](const me::order_rejected& x)     { o << "REJECTED         id=" << x.id << " reason=" << me::to_string(x.reason); },
        [&](const me::order_rested& x)       { o << "RESTED           id=" << x.id << " " << me::to_string(x.side) << " " << x.qty << "@" << x.price; },
        [&](const me::trade_event& x)        { o << "TRADE            #"   << x.trade_id << " " << x.qty << "@" << x.price
                                                                           << " taker=" << x.taker_id << "(" << x.taker_account << "," << me::to_string(x.taker_side)
                                                                           << ",leaves=" << x.taker_leaves << ")"
                                                                           << " maker=" << x.maker_id << "(" << x.maker_account << ",leaves=" << x.maker_leaves << ")"; },
        [&](const me::order_cancelled& x)    { o << "CANCELLED        id=" << x.id << " qty=" << x.cancelled_qty << " reason=" << me::to_string(x.reason); },
        [&](const me::cancel_rejected& x)    { o << "CANCEL_REJECTED  id=" << x.id << " reason=" << me::to_string(x.reason); },
        [&](const me::stp_policy_changed& x) { o << "CONFIG           stp=" << me::to_string(x.policy); },
    }, e);

    return o.str();
}

void render_top(std::ostream& out, const me::engine_state& s) 
{
    const me::top_of_book_view t = me::top_of_book(s);

    auto side = [](const std::optional<me::level_view>& l, const std::optional<me::orderId_t>& first) {

        if(!l) return std::string("--");

        return std::to_string(l->qty) + "@" + std::to_string(l->price) + " (" +
               std::to_string(l->order_count) + " ord, first=" + std::to_string(*first) + ")";

    };

    out << "  TOP " << s.book.symbol << "  bid " << side(t.bid, t.first_bid_id)  << "  |  ask " << side(t.ask, t.first_ask_id) << "\n";
}

void render_depth(std::ostream& out, const me::engine_state& s, bool orders) 
{
    if(!orders) 
    {
        // classic ladder: asks from worst to best on top, bids best to worst below,  the spread sits in the middle.
        const me::book_depth_view d = me::depth(s);

        out << "  DEPTH " << s.book.symbol << "\n";

        for (auto it = d.asks.rbegin(); it != d.asks.rend(); ++it)
            out << "    ASK " << it->price << "  qty=" << it->qty << "  orders=" << it->order_count << "\n";

        out << "    ----------\n";
        for (const auto& l : d.bids)
            out << "    BID " << l.price << "  qty=" << l.qty << "  orders=" << l.order_count << "\n";

        return;
    }

    // Order level, each side listed in the exact sequence it would be matched.
    const me::order_depth_view d = me::order_depth(s);
    out << "  DEPTH ORDERS " << s.book.symbol << "  (matching priority, first = next to fill)\n";
    auto side = [&](const char* name, const std::vector<me::resting_order_view>& v) {
        out << "    " << name << ":";

        if (v.empty()) out << " (empty)";
            out << "\n";

        for (std::size_t i = 0; i < v.size(); ++i)
            out << "      " << i + 1 << ". id=" << v[i].id << " acct=" << v[i].account << " " << v[i].leaves << "@" << v[i].price << " seq=" << v[i].seq << "\n";
    };
    
    side("ASKS", d.asks);
    side("BIDS", d.bids);
}

void render_status(std::ostream& out, const me::engine_state& s, me::orderId_t id) 
{
    auto o = me::order_status(s, id);

    if(!o) 
    { 
        out << "  STATUS id=" << id << " NOT_FOUND\n"; 
        
    } else {

        out << "  STATUS id=" << o->id << " acct=" << o->account << " " << me::to_string(o->side)
            << " " << me::to_string(o->type) << " " << me::to_string(o->tif) << " px=" << o->price
            << " qty=" << o->original_qty << " filled=" << o->filled_qty << " leaves=" << o->leaves_qty
            << " cancelled=" << o->cancelled_qty << " seq=" << o->seq
            << " status=" << me::to_string(o->status) << "\n";
    }
}

// ---------------------------------------------------------------------------
// session: owns the current state and threads it through me::apply().
// ---------------------------------------------------------------------------
class session 
{

public:
    session(std::ostream& out, bool echo, bool check_invariants) : out_(&out), echo_(echo), check_(check_invariants) 
    {}

    // redirect output (used by `exec --journal`: rebuild silently, then print).
    void set_output(std::ostream& out, bool echo) { out_ = &out; echo_ = echo; }

    // returns false on a parse error or invariant violation.
    bool feed(const std::string& raw, const std::string& where) 
    {
        Line line;
        std::string err;
        const parse_status ps = parse_line(raw, line, err);
        if(ps == parse_status::blank) 
        {
            // echo comment lines so transcripts are self-explanatory.
            if(echo_ && raw.find('#') != std::string::npos) *out_ << raw << "\n";

            return true;
        }

        if (ps == parse_status::error) 
        {
            std::cerr << where << ": parse error: " << err << "\n";
            return false;
        }
        
        if (echo_) *out_ << "> " << trim(raw) << "\n";

        bool ok = true;
        std::visit(overloaded{[&](const me::command& c) 
            {
                // std::move -> no copy of the book.
                me::step_result r = me::apply(std::move(state_), c);
                state_ = std::move(r.state);

                for(const me::event_t& e : r.events) *out_ << "  " << render(e) << "\n";

                ++commands_;

                if(check_) 
                {
                    if (auto bad = me::check_consistency(state_)) 
                    {
                        std::cerr << where << ": INVARIANT VIOLATION: " << *bad << "\n";
                        ok = false;
                    }
                }
            },
            [&](const QueryTop&) { render_top(*out_, state_); },
            [&](const QueryDepth& q) { render_depth(*out_, state_, q.orders); },
            [&](const QueryStatus& q) { render_status(*out_, state_, q.id); },

        }, line);

        return ok;
    }

    const me::engine_state& state() const 
    { return this->state_; }

    std::uint64_t commands() const 
    { return this->commands_; }

    static bool is_mutating(const std::string& raw)
    {
        Line line;
        std::string err;

        return parse_line(raw, line, err) == parse_status::ok && std::holds_alternative<me::command>(line);
    }

 private:

    static std::string trim(const std::string& s) 
    {
        const auto b = s.find_first_not_of(" \t\r");
        const auto e = s.find_last_not_of(" \t\r");
        return b == std::string::npos ? "" : s.substr(b, e - b + 1);
    }

    std::ostream* out_;  // pointer (not reference) so it can be redirected
    bool echo_;
    bool check_;
    me::engine_state state_ = me::make_engine();
    std::uint64_t commands_ = 0;
};

// Feed every line of a file into an existing session. Stops at the first
// parse error or invariant violation: scenario files are test inputs, and a
// typo that silently skipped a command would make the test meaningless.
int feed_file(session& session, const std::string& path) 
{
    std::ifstream in(path);
    if(!in)
    { 
        std::cerr << "cannot open " << path << "\n"; 
        return 2; 
    }
    
    std::string raw;
    std::size_t n = 0;

    while (std::getline(in, raw))
        if(!session.feed(raw, path + ":" + std::to_string(++n))) 
            return 1;
  
    return 0;
}

int run_replay(const std::string& path) 
{
    session s(std::cout, /*echo=*/true, /*check=*/true);

    return feed_file(s, path);
}

// FNV-1a 64: tiny, dependency-free, stable across platforms. Printed by  `verify` so transcripts from differrent builds can be compared by eye (same hash => same fills and book states).
std::uint64_t fnv1a(const std::string& s) 
{
    std::uint64_t h = 1469598103934665603ULL;

    for(unsigned char c : s) 
    {   
        h ^= c; h *= 1099511628211ULL; 
    }

    return h;
}

// verify: run the same file twice in fresh sessions, with the full consistency check after every command, and compare transcripts byte for byte. It also
// appends a final order-level snapshot so the final BOOK STATE is covered  the comparison, not just the fills. 
int run_verify(const std::string& path) 
{
    std::string transcripts[2];

    for(int i = 0; i < 2; ++i) 
    {
        std::ostringstream out;
        session s(out, /*echo=*/true, /*check=*/true);  // fresh state each run

        if(int rc = feed_file(s, path); rc != 0) 
            return rc;
        
        render_depth(out, s.state(), /*orders=*/true);
        transcripts[i] = out.str();
        
        if (i == 0) {

            const me::book_state& b = s.state().book;

            std::cout << "commands applied : " << s.commands() << "\n"
                      << "orders known     : " << b.orders.size() << "\n"
                      << "trades           : " << b.next_trade_id - 1 << "\n"
                      << "consistency      : checked after every command, all OK\n";
        }
    }

    const bool are_the_same = transcripts[0] == transcripts[1];
    std::cout << "transcript bytes : " << transcripts[0].size() << "\n"
              << "transcript fnv1a : " << std::hex << fnv1a(transcripts[0]) << std::dec << "\n"
              << "deterministic    : " << (are_the_same ? "YES (run 1 == run 2)" : "NO -- MISMATCH") << "\n";

    return are_the_same ? 0 : 1;
}

int run_repl(void) 
{
    session s(std::cout, /*echo=*/false, /*check=*/true);
    std::cout << "matching engine REPL. Type commands (PLACE/CANCEL/CONFIG/TOP/DEPTH/STATUS), Ctrl-D to quit.\n";

    std::string raw;
    std::size_t n = 0;
  
    while (std::cout << "me> " << std::flush, std::getline(std::cin, raw))
        s.feed(raw, "stdin:" + std::to_string(++n));  // errors reported, session continues
  
    return 0;
}

// exec with an optional journal = event sourcing in miniature. The journal stores input commands, never outputs or snapshots; the book is rebuilt
// by replaying it through the pure function. This is only correct because  the engine is deterministic -- a nice live demonstration of a requirement.
// Tradeoff: startup cost grows with the journal length. 
int run_exec(const std::vector<std::string>& args) 
{
    std::string journal;
    std::size_t i = 0;
    if(i < args.size() && args[i] == "--journal") 
    {
        if(i + 1 >= args.size()) 
        { 
            std::cerr << "--journal needs a file\n"; return 2; 
        }

        journal = args[i + 1];
        i += 2;
    }

    // step 1: rebuild the book by replaying the journal with output discarded.
    std::ostream null_out(nullptr);  // stream without a buffer swallows writes
    session s(null_out, /*echo=*/false, /*check=*/true);
    if(!journal.empty()) 
    {
        std::ifstream probe(journal);
        // a missing journal simply means "start with an empty book"
        if(probe) {  
            probe.close();
            if(int rc = feed_file(s, journal); rc != 0) 
                return rc;
        }
    }

    // step 2: apply the new commands with output on, journaling the mutating ones. We journal commands even if the me rejects them: replay will
    // reject them again, and "log every input" is the simplest rule that keeps the journal a faithful record. Lines that fail to parse  are not journaled
  
    s.set_output(std::cout, /*echo=*/true);
    std::ofstream append;
    if(!journal.empty()) 
        append.open(journal, std::ios::app);
  
    int rc = 0;
    for(; i < args.size(); ++i) 
    {
        if(!s.feed(args[i], "arg")) 
            rc = 1;             
        else if(append && session::is_mutating(args[i])) 
            append << args[i] << "\n";
    }

    return rc;
}

// Reproducible random command stream. std::uniform_int_distribution is deliberately avoided: its algorithm is  implementation-defined, so the same seed would give different 
// streams on libstdc++ vs libc++ vs MSVC.
int run_gen(std::uint64_t seed, std::uint64_t count) 
{

    std::mt19937_64 rng(seed);
    auto pick = [&](std::uint64_t k) { return rng() % k; };

    const char* accounts[] = {"A", "B", "C", "D", "E"};
    const char* policies[] = {"CANCEL_INCOMING", "CANCEL_RESTING", "CANCEL_BOTH"};

    std::uint64_t next_id = 1;
    std::cout << "# generated: seed=" << seed << " count=" << count << "\n";
    
    for(std::uint64_t n = 0; n < count; ++n) 
    {
        const auto r = pick(100);

        if(r < 60 || next_id == 1)
        {
            const bool buy    = pick(2)  == 0;
            const bool market = pick(20) == 0;
            const bool ioc    = pick(6)  == 0;
      
            // buys centred slightly below sells so the book builds depth instead of every order crossing immediately.
            const std::int64_t px = buy ? 95 + static_cast<std::int64_t>(pick(9))
                                        : 97 + static_cast<std::int64_t>(pick(9));
        
            std::cout << "PLACE " << next_id++ << " " << accounts[pick(5)] << " "
                    << (buy ? "BUY " : "SELL ") << (market ? "MARKET 0 " : "LIMIT ")
                    << (market ? "" : std::to_string(px) + " ") << 1 + pick(20) << " "
                    << (ioc ? "IOC" : "GTC") << "\n";
        }else if(r < 97) {
            // mostly target recent orders (likely still resting -> real cancels and tombstones), sometimes any id 
            const std::uint64_t window = std::min<std::uint64_t>(next_id - 1, 40);
            const std::uint64_t id = pick(4) == 0 ? 1 + pick(next_id + 5)
                                            : next_id - 1 - pick(window);
            std::cout << "CANCEL " << id << "\n";
        }else if(r < 98) {
            std::cout << "CONFIG STP " << policies[pick(3)] << "\n";
        }else{

            std::cout << "TOP\n";
        }
    }

    std::cout << "DEPTH\n";
    return 0;
}

void usage() 
{
    std::cout <<
      "usage:\n"
      "  me replay <file>                 run scenario file, print all events\n"
      "  me verify <file>                 run twice with invariant checks; prove determinism\n"
      "  me repl                          interactive session on stdin\n"
      "  me exec [--journal FILE] CMD...  apply commands given as arguments\n"
      "  me gen <seed> <count>            emit a reproducible random command stream\n"
      "\n"
      "commands:\n"
      "  PLACE <id> <acct> <BUY|SELL> <LIMIT|MARKET> <price> <qty> [GTC|IOC]\n"
      "  CANCEL <id> | CONFIG STP <CANCEL_INCOMING|CANCEL_RESTING|CANCEL_BOTH>\n"
      "  TOP | DEPTH [ORDERS] | STATUS <id>\n";
}

}  // namespace

int main(int argc, char** argv) 
{
    if (argc < 2)
    { 
        usage(); 
        return -1; 
    }
    
    std::string mode = argv[1];
    std::vector<std::string> rest(argv + 2, argv + argc);

    if(mode == "replay" && rest.size() == 1) return run_replay(rest[0]);
    if(mode == "verify" && rest.size() == 1) return run_verify(rest[0]);
    if(mode == "repl"   && rest.empty())     return run_repl();
    if(mode == "exec"   && !rest.empty())    return run_exec(rest);
    if(mode == "gen"    && rest.size() == 2) 
    {
        std::uint64_t seed = 0, count = 0;

        if(!parse_int(rest[0], seed) || !parse_int(rest[1], count)) 
        { 
            usage(); return -1; 
        }
    
        return run_gen(seed, count);
    }
    
    usage();
    return -1;
}