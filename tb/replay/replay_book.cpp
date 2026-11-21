// replays a full NASDAQ day through one BookModel per symbol
// 1. proves the model holds up on real data (0 misses, no assert fires)
// 2. measures what the RTL book has to fit (peak orders, top-10 orders, levels, level size)
/*
clang++ -std=c++17 -O2 -Wall -Wextra -o tb/replay/replay_book tb/replay/replay_book.cpp tb/replay/itch_file_reader.cpp -lz
./tb/replay/replay_book ~/data/itch/12302019.NASDAQ_ITCH50.gz
*/

#include "itch_file_reader.hpp"
#include "../book_model.hpp"
#include "../itch_messages.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <vector>

// stock_locate is a 16b field, so 2^16 = 65536 possible symbols
// one book per locate, indexed directly (same idea as using stock_locate as a RAM address in hardware)
static constexpr size_t NUM_LOCATES = 1 << 16;

// the on-chip top-of-book window from the design (same 10 as the Building the Book paper's L2)
static constexpr size_t TOP_LEVELS = 10;

// RTL limits today (msg_pkg.sv), to compare the real peaks against
static constexpr size_t RTL_L3_ORDERS = 4096; // 1024 buckets * 4 entries
static constexpr size_t RTL_L2_SLOTS = 1024; // price ladder slots per side
static constexpr uint64_t RTL_LEVEL_SHARES_MAX = 0xFFFFFFFF; // 32b level total

// how many of the busiest symbols to list at the end
static constexpr size_t NUM_TO_LIST = 10;

// progress line every 50M messages; a full day is ~270M
static constexpr uint64_t PROGRESS_EVERY = 50000000;

struct SymbolStats{
    char ticker[9] = {}; // 8 chars from the R message + '\0'
    uint64_t messages = 0; // book-changing messages for this symbol
    size_t peak_orders = 0; // most live orders at once
    size_t peak_top_orders = 0; // most orders at once in the top 10 levels (both sides)
    size_t peak_levels_per_side = 0; // most price levels at once on either side
};

static bool is_book_changing(uint8_t type){
    switch (type){
        case 'A': case 'F': case 'E': case 'C': case 'X': case 'D': case 'U':
            return true;
        default:
            return false;
    }
}

// R (stock directory): stock symbol @11, 8 chars right-padded with spaces
static void save_ticker(SymbolStats &s, const std::vector<uint8_t> &msg){
    for (int i = 0; i < 8; i++){
        char c = (char)msg[11 + i];
        s.ticker[i] = (c == ' ') ? '\0' : c; // drop the padding
    }
    s.ticker[8] = '\0';
}

// pull fields out of one book-changing message and apply it
// offsets are straight from the ITCH 5.0 spec tables
static void apply_to_book(BookModel &book, const std::vector<uint8_t> &msg){
    switch (msg[0]){
        case 'A':
        case 'F':{
            uint64_t ref = read_u64_bigendian(&msg[11]); //@11
            bool is_buy = (msg[19] == 'B'); //@19 'B' or 'S'
            uint32_t shares = read_u32_bigendian(&msg[20]); //@20
            uint32_t price = read_u32_bigendian(&msg[32]); //@32 (F's MPID @36 is ignored)
            book.add_order(ref, is_buy, price, shares);
            break;
        }
        case 'E':
        case 'C':{
            uint64_t ref = read_u64_bigendian(&msg[11]); //@11
            uint32_t shares = read_u32_bigendian(&msg[19]); //@19 executed shares
            book.exc_order(ref, shares); // C's execution price @32 doesn't move the book
            break;
        }
        case 'X':{
            uint64_t ref = read_u64_bigendian(&msg[11]); //@11
            uint32_t shares = read_u32_bigendian(&msg[19]); //@19 cancelled shares
            book.cancel_order(ref, shares);
            break;
        }
        case 'D':{
            uint64_t ref = read_u64_bigendian(&msg[11]); //@11
            book.del_order(ref);
            break;
        }
        case 'U':{
            uint64_t old_ref = read_u64_bigendian(&msg[11]); //@11 original order
            uint64_t new_ref = read_u64_bigendian(&msg[19]); //@19 new order
            uint32_t shares = read_u32_bigendian(&msg[27]); //@27 new total shares
            uint32_t price = read_u32_bigendian(&msg[31]); //@31 new price
            book.replace_order(old_ref, new_ref, price, shares);
            break;
        }
        default:
            ASSERT(false); // caller only sends the 7 book-changing types
    }
}

int main(int argc, char **argv){
    if (argc != 2){
        std::fprintf(stderr, "usage: %s <NASDAQ_ITCH50.gz>\n", argv[0]);
        return 2;
    }

    auto t0 = std::chrono::steady_clock::now();

    // heap, not stack: 65536 books + stats is too big for the stack
    std::vector<BookModel> books(NUM_LOCATES);
    std::vector<SymbolStats> stats(NUM_LOCATES);

    uint64_t applied[256] = {}; // book-changing messages applied, by type byte
    size_t market_live = 0; // live orders across every symbol right now
    size_t market_peak = 0;

    try{
        ItchFileReader rd(argv[1]);
        std::vector<uint8_t> msg;

        while (rd.next(msg)){
            uint8_t type = msg[0];
            uint16_t locate = read_u16_bigendian(&msg[1]); //@1 on every ITCH message

            if (type == 'R'){
                save_ticker(stats[locate], msg);
                continue;
            }
            if (!is_book_changing(type)){
                continue;
            }

            BookModel &book = books[locate];
            size_t orders_before = book.live_orders();

            apply_to_book(book, msg);
            applied[type]++;

            // per-symbol peaks
            SymbolStats &s = stats[locate];
            s.messages++;
            s.peak_orders = std::max(s.peak_orders, book.live_orders());
            s.peak_top_orders = std::max(s.peak_top_orders, book.orders_in_top_levels(TOP_LEVELS));
            s.peak_levels_per_side = std::max(s.peak_levels_per_side, book.bid_level_count());
            s.peak_levels_per_side = std::max(s.peak_levels_per_side, book.ask_level_count());

            // whole market: only this one book changed, so shift the total by its difference
            // (add first, then subtract, so the unsigned count never dips below 0)
            market_live += book.live_orders();
            market_live -= orders_before;
            market_peak = std::max(market_peak, market_live);

            if (rd.count() % PROGRESS_EVERY == 0){
                std::fprintf(stderr, "  %llu messages...\n", (unsigned long long)rd.count());
            }
        }
    }
    catch (const std::exception &e){
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }

    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // roll up the per-symbol numbers
    uint64_t total_misses = 0;
    uint64_t peak_level_shares = 0;
    size_t widest_side = 0;
    size_t symbols_over_l3 = 0;
    size_t symbols_over_l2 = 0;
    std::vector<size_t> active; // locates that saw at least 1 book-changing message

    for (size_t loc = 0; loc < NUM_LOCATES; loc++){
        total_misses += books[loc].get_miss_count();
        peak_level_shares = std::max(peak_level_shares, books[loc].get_peak_level_shares());

        if (stats[loc].messages == 0){
            continue;
        }
        active.push_back(loc);
        widest_side = std::max(widest_side, stats[loc].peak_levels_per_side);

        if (stats[loc].peak_orders > RTL_L3_ORDERS){
            symbols_over_l3++;
        }
        if (stats[loc].peak_levels_per_side > RTL_L2_SLOTS){
            symbols_over_l2++;
        }
    }

    std::printf("applied:");
    for (char t : {'A', 'F', 'E', 'C', 'X', 'D', 'U'}){
        std::printf(" %c=%llu", t, (unsigned long long)applied[(uint8_t)t]);
    }
    std::printf("\n");

    std::printf("misses: %llu (0 expected: the day starts with an empty book)\n", (unsigned long long)total_misses);
    std::printf("symbols with book activity: %zu\n", active.size());
    std::printf("market live orders: peak %zu, at close %zu\n", market_peak, market_live);
    std::printf("largest level: %llu shares (RTL max %llu)\n",
        (unsigned long long)peak_level_shares, (unsigned long long)RTL_LEVEL_SHARES_MAX);
    std::printf("widest book side: %zu levels (RTL ladder %zu per side)\n", widest_side, RTL_L2_SLOTS);
    std::printf("symbols over RTL L3 (%zu orders): %zu\n", RTL_L3_ORDERS, symbols_over_l3);
    std::printf("symbols over RTL L2 (%zu levels a side): %zu\n", RTL_L2_SLOTS, symbols_over_l2);

    // busiest symbols by peak live orders
    // partial_sort only sorts the first n (the rest stay in any order)
    // the [&](...){...} is a lambda: an inline compare function, [&] lets it see stats
    size_t n = std::min(NUM_TO_LIST, active.size());
    std::partial_sort(active.begin(), active.begin() + n, active.end(), [&](size_t a, size_t b){
        return stats[a].peak_orders > stats[b].peak_orders;
    });

    std::printf("\ntop %zu symbols by peak live orders:\n", n);
    std::printf("ticker locate peak_orders peak_top%zu_orders peak_levels_per_side messages\n", TOP_LEVELS);
    for (size_t i = 0; i < n; i++){
        const SymbolStats &s = stats[active[i]];
        std::printf("%s %zu %zu %zu %zu %llu\n", s.ticker, active[i], s.peak_orders,
            s.peak_top_orders, s.peak_levels_per_side, (unsigned long long)s.messages);
    }

    std::printf("\nelapsed: %.1f s\n", secs);
    std::printf("%s\n", (total_misses == 0) ? "PASSED (every E/C/X/D/U found its order)" : "FAILED (misses above 0)");

    return (total_misses == 0) ? 0 : 1;
}
