// replays a full Nasdaq day through one BookModel per symbol
// 1. proves the model holds up on real data (0 misses, no assert fires)
// 2. measures what the RTL book has to fit (peak orders, top-10 orders, levels, level size)
// 3. mirrors the RTL's hashed buckets + ladder per symbol to find which symbols the RTL holds all day
/*
clang++ -std=c++17 -O2 -Wall -Wextra -o tb/replay/replay_book tb/replay/replay_book.cpp tb/replay/itch_file_reader.cpp -lz
./tb/replay/replay_book ~/data/itch/12302019.NASDAQ_ITCH50.gz
*/

#include "itch_file_reader.hpp"
#include "itch_to_model.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <memory>
#include <vector>

// stock_locate is a 16b field, so 2^16 = 65536 possible symbols
// one book per locate, indexed directly (same idea as using stock_locate as a RAM address in hardware)
static constexpr size_t NUM_LOCATES = 1 << 16;

// the on-chip top-of-book window from the design (same 10 as the Building the Book paper's L2)
static constexpr size_t TOP_LEVELS = 10;

// RTL limits today (msg_pkg.sv), to compare the real peaks against
static constexpr size_t RTL_NUM_BUCKETS = 1024; // NUM_BUCKETS (2^BOOK_ADDR_WIDTH)
static constexpr size_t RTL_BUCKET_WIDTH = 4; // ENTRIES_PER_BUCKET
static constexpr size_t RTL_L3_ORDERS = RTL_NUM_BUCKETS * RTL_BUCKET_WIDTH; // 4096
static constexpr size_t RTL_L2_SLOTS = 1024; // price ladder slots per side (NUM_LEVELS)
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

// RTL FIT
// the RTL hashes instead of sorting, so it can run out of room way before 4096 orders:
// - a 5th order in 1 bucket gets dropped (overflow)
// - 2 live prices that hash to the same ladder slot means 1 of them has no level (collision)
// so this mirrors the RTL's hashing for 1 symbol to see if it'd make it through the whole day with neither

// same as hash_order_ref in msg_pkg.sv: XOR the 4 16b chunks, keep the low 10 bits
static uint32_t rtl_bucket(uint64_t ref){
    uint16_t folded = (uint16_t)(ref ^ (ref>>16) ^ (ref>>32) ^ (ref>>48));
    return folded % RTL_NUM_BUCKETS;
}

// same as hash_price in msg_pkg.sv: XOR the 2 16b halves, keep the low 10 bits
static uint32_t rtl_slot(uint32_t price){
    uint16_t folded = (uint16_t)(price ^ (price>>16));
    return folded % RTL_L2_SLOTS;
}

struct RtlFit{
    uint8_t bucket_fill[RTL_NUM_BUCKETS] = {}; // live orders in each bucket
    uint32_t bid_slots[RTL_L2_SLOTS] = {}; // price that owns each ladder slot, 0 = empty (no legal price is 0)
    uint32_t ask_slots[RTL_L2_SLOTS] = {};
    bool overflowed = false;
    bool collided = false;

    bool fits(){
        return !overflowed && !collided;
    }
};

// an order joins the book: needs a free entry in its bucket, and a ladder slot if its price is a new level
static void fit_add(RtlFit &fit, uint64_t ref, bool is_buy, uint32_t price, bool level_existed){
    uint32_t b = rtl_bucket(ref);
    if (fit.bucket_fill[b] == RTL_BUCKET_WIDTH){
        fit.overflowed = true; // RTL drops this add
        return;
    }
    fit.bucket_fill[b]++;

    if (!level_existed){
        uint32_t *slots = is_buy ? fit.bid_slots : fit.ask_slots;
        uint32_t s = rtl_slot(price);
        if (slots[s] != 0){
            fit.collided = true; // a different live price already owns this slot
            return;
        }
        slots[s] = price;
    }
}

// an order leaves the book: frees its bucket entry, and its ladder slot if it was the level's last order
static void fit_remove(RtlFit &fit, uint64_t ref, bool is_buy, uint32_t price, bool level_gone){
    fit.bucket_fill[rtl_bucket(ref)]--;

    if (level_gone){
        uint32_t *slots = is_buy ? fit.bid_slots : fit.ask_slots;
        slots[rtl_slot(price)] = 0;
    }
}

// applies 1 book-changing msg to the model AND the RTL mirror
// (looks at the order before and after so it knows if the order or its level came or went)
static void apply_with_fit(BookModel &book, RtlFit &fit, const std::vector<uint8_t> &msg){
    uint8_t type = msg[0];
    uint64_t ref = read_u64_bigendian(&msg[11]); //@11 (U: the old order)

    // before
    bool old_is_buy = false;
    uint32_t old_price = 0;
    bool had_order = book.find_order(ref, old_is_buy, old_price);

    bool add_is_buy = (msg[19] == 'B'); //@19 A/F only
    uint32_t add_price = (type == 'A' || type == 'F') ? read_u32_bigendian(&msg[32]) : 0; //@32 A/F only
    uint32_t rpl_price = (type == 'U') ? read_u32_bigendian(&msg[31]) : 0; //@31 U only
    bool add_level_existed = book.has_level(add_is_buy, add_price);
    bool rpl_level_existed = had_order && book.has_level(old_is_buy, rpl_price);

    apply_to_book(book, msg);

    // once the RTL drops something it no longer matches the model, so stop mirroring
    if (!fit.fits()){
        return;
    }

    // after
    switch (type){
        case 'A':
        case 'F':
            fit_add(fit, ref, add_is_buy, add_price, add_level_existed);
            break;
        case 'E':
        case 'C':
        case 'X':
        case 'D':
            // a partial E/C/X doesn't change any structure, only a fully gone order does
            if (had_order && !book.has_order(ref)){
                fit_remove(fit, ref, old_is_buy, old_price, !book.has_level(old_is_buy, old_price));
            }
            break;
        case 'U':
            // same order as the RTL: delete half first, then the add half (so a slot freed by the delete is reusable)
            if (had_order){
                uint64_t new_ref = read_u64_bigendian(&msg[19]); //@19
                fit_remove(fit, ref, old_is_buy, old_price, !book.has_level(old_is_buy, old_price));
                fit_add(fit, new_ref, old_is_buy, rpl_price, rpl_level_existed);
            }
            break;
        default:
            break;
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

int main(int argc, char **argv){
    if (argc != 2){
        std::fprintf(stderr, "usage: %s <NASDAQ_ITCH50.gz>\n", argv[0]);
        return 2;
    }

    auto t0 = std::chrono::steady_clock::now();

    // heap, not stack: 65536 books + stats is too big for the stack
    std::vector<BookModel> books(NUM_LOCATES);
    std::vector<SymbolStats> stats(NUM_LOCATES);
    std::vector<std::unique_ptr<RtlFit>> fits(NUM_LOCATES); // made on a symbol's 1st book msg (~9 KB each, ~8,900 symbols trade)

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

            if (!fits[locate]){
                fits[locate] = std::make_unique<RtlFit>();
            }
            apply_with_fit(book, *fits[locate], msg);
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

    // which symbols the RTL's hashed book holds all day
    std::vector<size_t> fitting;
    size_t dropped_order = 0;
    size_t lost_level = 0;
    for (size_t loc : active){
        if (fits[loc]->overflowed){
            dropped_order++;
        }
        else if (fits[loc]->collided){
            lost_level++;
        }
        else{
            fitting.push_back(loc);
        }
    }
    std::printf("\nsymbols the RTL holds all day: %zu of %zu\n", fitting.size(), active.size());
    std::printf("  failed on a full bucket (5th order in 1 bucket gets dropped): %zu\n", dropped_order);
    std::printf("  failed on a ladder collision (2 live prices in 1 slot): %zu\n", lost_level);

    // the busiest of those = the best real-day test for the RTL as it is today
    size_t f = std::min(NUM_TO_LIST, fitting.size());
    std::partial_sort(fitting.begin(), fitting.begin() + f, fitting.end(), [&](size_t a, size_t b){
        return stats[a].messages > stats[b].messages;
    });
    std::printf("busiest %zu the RTL holds all day:\n", f);
    std::printf("ticker locate messages peak_orders peak_levels_per_side\n");
    for (size_t i = 0; i < f; i++){
        const SymbolStats &s = stats[fitting[i]];
        std::printf("%s %zu %llu %zu %zu\n", s.ticker, fitting[i], (unsigned long long)s.messages,
            s.peak_orders, s.peak_levels_per_side);
    }

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
