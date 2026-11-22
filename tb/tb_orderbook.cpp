/*
rm -rf obj_dir
verilator --cc --exe --build -j 0  --assert +define+SIM --top-module order_book rtl/msg_pkg.sv rtl/order_book.sv tb/tb_orderbook.cpp
./obj_dir/Vorder_book
*/

#include "Vorder_book.h"
#include <verilated.h>
#include <cstdio>
#include <cstdint>
#include "contracts.hpp"
#include <vector>
#include <map>
#include "book_model.hpp"
#include <cstdlib>

static int failures = 0;
static constexpr int RESET_CYCLES = 5;
static constexpr int TDATA_WORDS = 15; // 480b = 32b * 15 words

// match msg_pkg
static constexpr uint8_t MSG_NONE = 0;
static constexpr uint8_t MSG_ADD = 1;
static constexpr uint8_t MSG_EXC = 2;
static constexpr uint8_t MSG_DEL = 3;
static constexpr uint8_t MSG_CXL = 4;
static constexpr uint8_t MSG_RPL = 5;
static constexpr uint8_t MSG_ACT = 6;
static constexpr uint8_t MSG_OPH = 7;

static constexpr uint32_t ASK_EMPTY = 0xFFFFFFFF;
static constexpr uint32_t BID_EMPTY = 0;

static void tick(Vorder_book *dut){
    REQUIRES(dut != nullptr);
    //

    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
    
    //
    ENSURES(dut->clk == 1);
}

static void reset(Vorder_book *dut)
{
    REQUIRES(dut != nullptr);
    //
    
    dut->rst_n = 0;
    dut->s_axis_tvalid = 0;
    dut->cfg_stock_locate = 1; // all tests use stock_locate=1 here

    for (int i = 0; i < TDATA_WORDS; i++){
        dut->s_axis_tdata[i] = 0;
    }

    for (int i = 0; i < RESET_CYCLES; i++){
        tick(dut);
    }

    dut->rst_n = 1;
    tick(dut);

    //
    ENSURES(dut->rst_n == 1);
    ENSURES(dut->s_axis_tvalid == 0);
}


static void check(uint64_t actual, uint64_t expected, const char *name){
    REQUIRES(name != nullptr);
    //
    
    if (actual != expected){
        std::printf("FAIL %-28s got: 0x%llx, expected: 0x%llx\n",
                    name, (unsigned long long)actual, (unsigned long long)expected);
        failures++;
    }
}

static Vorder_book *fresh_dut(){
    Vorder_book *dut = new Vorder_book;
    reset(dut);

    ENSURES(dut != nullptr);
    return dut;
}

// mirrors msg_pkg.sv
static void setup_msg(
    Vorder_book *dut,
    uint8_t msg_type,
    bool is_buy,
    uint16_t stock_locate,
    uint64_t order_ref,
    uint32_t shares,
    uint32_t price
){
    REQUIRES(dut != nullptr);
    //

    for (int i = 0; i < TDATA_WORDS; i++){
        dut->s_axis_tdata[i] = 0;
    }

    dut->s_axis_tdata[2] = price;
    dut->s_axis_tdata[5] = shares;
    dut->s_axis_tdata[6] = (uint32_t)(order_ref & 0xFFFFFFFF);
    dut->s_axis_tdata[7] = (uint32_t)(order_ref>>32);
    dut->s_axis_tdata[10] = (uint32_t)stock_locate;

    if (is_buy){
        dut->s_axis_tdata[11] = (uint32_t)msg_type | ((uint32_t)1 <<4);
    }
    else{
        dut->s_axis_tdata[11] = (uint32_t)msg_type | ((uint32_t)0 <<4);
    }
}

// hands whatever setup_msg put on s_axis_tdata to the book, then waits for it to get back to IDLE
// returns how many times the book published (book_valid pulses) while working on that msg
static int send_and_wait(Vorder_book *dut, uint64_t order_ref, int max_wait){
    REQUIRES(dut != nullptr);
    REQUIRES(dut->rst_n == 1);
    //

    dut->s_axis_tvalid = 1;
    
    // wait for handshake
    bool handshake = false;
    for (int i = 0; i < max_wait && !handshake; i++){
        dut->eval();
        handshake = dut->s_axis_tready;
        tick(dut);
    }
    if (!handshake){
        std::printf("FAILED: book did not accept message; order_ref: %llu\n",
                    (unsigned long long)order_ref);
        failures++;
        return 0;
    }

    // wait for processing
    dut->s_axis_tvalid = 0;
    int pulses = 0;
    for (int i = 0; i < max_wait; i++){
        dut->eval();
        if (dut->s_axis_tready){
            return pulses;
        }
        tick(dut);
        if (dut->book_valid){
            pulses++;
        }
    }
    std::printf("FAILED: book did not return to idle state; order_ref: %llu\n",
                (unsigned long long)order_ref);
    failures++;
    return pulses;
}

static int push_msg(
    Vorder_book *dut,
    uint8_t msg_type,
    bool is_buy,
    uint16_t stock_locate,
    uint64_t order_ref,
    uint32_t shares,
    uint32_t price,
    int max_wait = 2000
){
    REQUIRES(dut != nullptr);
    //

    setup_msg(dut, msg_type, is_buy, stock_locate, order_ref, shares, price);
    return send_and_wait(dut, order_ref, max_wait);
}

// U is the only msg with a 2nd ref (words 12-13), so it gets its own helper
static int push_replace(
    Vorder_book *dut,
    uint16_t stock_locate,
    uint64_t orig_ref,
    uint64_t new_ref,
    uint32_t shares, // new TOTAL
    uint32_t price, // new price
    bool stale_is_buy, // U has no side byte, so this is whatever the parser had left over (the book has to ignore it)
    int max_wait = 2000
){
    REQUIRES(dut != nullptr);
    //

    setup_msg(dut, MSG_RPL, stale_is_buy, stock_locate, orig_ref, shares, price);
    dut->s_axis_tdata[12] = (uint32_t)(new_ref & 0xFFFFFFFF);
    dut->s_axis_tdata[13] = (uint32_t)(new_ref>>32);

    return send_and_wait(dut, orig_ref, max_wait);
}

// H: trading_state sits in word 14 bits [23:16]
static int push_trading_action(Vorder_book *dut, uint16_t stock_locate, char trading_state){
    REQUIRES(dut != nullptr);
    //

    setup_msg(dut, MSG_ACT, false, stock_locate, 0, 0, 0);
    dut->s_axis_tdata[14] = (uint32_t)(uint8_t)trading_state << 16;

    return send_and_wait(dut, 0, 2000);
}

// h: market_code in word 14 bits [15:8], action in [7:0]
static int push_op_halt(Vorder_book *dut, uint16_t stock_locate, char market_code, char action){
    REQUIRES(dut != nullptr);
    //

    setup_msg(dut, MSG_OPH, false, stock_locate, 0, 0, 0);
    dut->s_axis_tdata[14] = ((uint32_t)(uint8_t)market_code << 8) | (uint32_t)(uint8_t)action;

    return send_and_wait(dut, 0, 2000);
}


/* 
actual tests:
*/

// empty book should give the max/min values for bid/ask
// need to be able to see no market vs market at price 0
static void test_reset(){
    std::printf("TEST1: empty book resets to max/min val\n");
    Vorder_book *dut = fresh_dut();

    check(dut->best_ask_price, ASK_EMPTY, "ask baseline");
    check(dut->best_bid_price, BID_EMPTY, "bid baseline");
    check(dut->miss_count, 0, "miss_count");
    check(dut->overflow_count, 0, "overflow_count");
    check(dut->level_collision_count, 0, "level_collision_count");

    dut->final();
    delete dut;
}

// one Add per side (bid/ask are on diff ladders)
static void test_add_sets_topofbook(){
    std::printf("TEST2: add sets top of book\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 100, 500, 1230000);
    check(dut->best_bid_price, 1230000, "bid price");
    check(dut->best_bid_shares, 500, "bid shares");
    push_msg(dut, MSG_ADD, false, 1, 200, 300, 1230500);
    check(dut->best_ask_price, 1230500, "ask price");
    check(dut->best_ask_shares, 300, "ask shares");
    check(dut->best_bid_price, 1230000, "ask doesn't affect bid");

    dut->final();
    delete dut;
}


// adding should only improve topofbook, a worse price should not affect it
static void test_add_only_improves_topofbook(){
    std::printf("TEST3: top of book does not worsen on an Add\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 100, 500, 1230000);
    check(dut->best_bid_price, 1230000, "bid 1");
    push_msg(dut, MSG_ADD, true, 1, 101, 400, 1229500);
    check(dut->best_bid_price, 1230000, "worse bid ignored");
    check(dut->best_bid_shares, 500, "shares remain from best bid)");
    push_msg(dut, MSG_ADD, true, 1, 102, 600, 1231000);
    check(dut->best_bid_price, 1231000, "better bid takes top");
    check(dut->best_bid_shares, 600, "better bid's shares replaces old");

    dut->final();
    delete dut;
}

// if we have multiple orders at the same price they should collapse to one L2 level
static void test_add_aggregates_same_price(){
    std::printf("TEST4: orders at same price collapse to one L2 level\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 100, 500, 1230000);
    check(dut->best_bid_shares, 500, "1 order");
    push_msg(dut, MSG_ADD, true, 1, 101, 300, 1230000);
    check(dut->best_bid_price, 1230000, "price same");
    check(dut->best_bid_shares, 800, "aggregated the 2 orders in the level");
    push_msg(dut, MSG_ADD, true, 1, 102, 200, 1230000);
    check(dut->best_bid_shares, 1000, "aggregated the 3 orders in the level");

    dut->final();
    delete dut;
}

// if execute a partial amount of a level, shares should drop but order and L2 level stay
static void test_exc_partial_reduces_shares(){
    std::printf("TEST5: partial filling reduces shares\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 100, 500, 1230000);
    push_msg(dut, MSG_EXC, false, 1, 100, 200, 0);
    check(dut->best_bid_price, 1230000, "price survives");
    check(dut->best_bid_shares, 300, "shares reduced by the amount executed");
    check(dut->miss_count, 0, "no misses");

    dut->final();
    delete dut;
}

static void test_exc_full_empties_level_and_rescans(){
    std::printf("TEST6: executed full empties the level and causes rescan for top level\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 100, 500, 1230000); // top
    push_msg(dut, MSG_ADD, true, 1, 101, 400, 1229500);
    check(dut->best_bid_price, 1230000, "top prior to emptying");
    push_msg(dut, MSG_EXC, false, 1, 100, 500, 0); // empties top level
    check(dut->best_bid_price, 1229500, "rescan moved 2nd best up");
    check(dut->best_bid_shares, 400, "2nd best's shares");

    dut->final();
    delete dut;
}

static void test_exc_unknown_ref_counts_miss(){
    std::printf("TEST7: executing on unknown order ref causes miss to increment");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_EXC, false, 1, 123, 100, 0);
    check(dut->miss_count, 1, "execute miss counted");
    check(dut->best_bid_price, BID_EMPTY, "book not affected by miss");

    dut->final();
    delete dut;
}

// deleting should remove full order, and deleting last order on bid/ask should reset it to baseline
static void test_del_removes_order_and_rescans(){
    std::printf("TEST8: delete removes full order\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 100, 500, 1230000);
    push_msg(dut, MSG_ADD, true, 1, 101, 400, 1229500);
    push_msg(dut, MSG_DEL, false, 1, 100, 0, 0);
    check(dut->best_bid_price, 1229500, "top removed and 2nd best promoted");
    check(dut->best_bid_shares, 400, "next levels shares");
    push_msg(dut, MSG_DEL, false, 1, 101, 0, 0);
    check(dut->best_bid_price, BID_EMPTY, "empty book gives 0 (baseline for bid)");

    dut->final();
    delete dut;
}

// when we delete one of multiple orders at an L2 level, it should not delete the level - like exc
static void test_del_leaves_level_existing(){
    std::printf("TEST9: delete leaves L2 level when there are other orders on it\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 100, 500, 1230000);
    push_msg(dut, MSG_ADD, true, 1, 101, 300, 1230000);
    check(dut->best_bid_shares, 800, "aggregated shares on add");
    push_msg(dut, MSG_DEL, false, 1, 100, 0, 0);
    check(dut->best_bid_price, 1230000, "L2 level survived delete");
    check(dut->best_bid_shares, 300, "shares reduced and L2 level survived");

    dut->final();
    delete dut;
}

static void test_del_unknown_ref_counts_miss(){
    std::printf("TEST10: delete on unknow order ref counts as a miss\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_DEL, false, 1, 123, 0, 0);
    check(dut->miss_count, 1, "delete miss counted");
    check(dut->best_bid_price, BID_EMPTY, "book not affected by miss");

    dut->final();
    delete dut;
}

// X shrinks the order but it stays in the book (and so does its level)
static void test_cxl_partial_keeps_order(){
    std::printf("TEST14: partial cancel shrinks the order and the level survives\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, false, 1, 100, 1000, 1230000);
    push_msg(dut, MSG_ADD, false, 1, 101, 200, 1230000);
    push_msg(dut, MSG_CXL, false, 1, 100, 400, 0); // 1000 -> 600 on ref 100
    check(dut->best_ask_price, 1230000, "level survived");
    check(dut->best_ask_shares, 800, "600 + 200 left at the level");

    // the order is still there, so a delete on it should find it (no miss)
    push_msg(dut, MSG_DEL, false, 1, 100, 0, 0);
    check(dut->best_ask_shares, 200, "only ref 101 left");
    check(dut->miss_count, 0, "ref 100 was still in the book after the X");

    dut->final();
    delete dut;
}

// an X for everything that's left clears the order like a delete would
static void test_cxl_full_empties_level_and_rescans(){
    std::printf("TEST15: cancelling every remaining share empties the level and causes rescan\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, false, 1, 100, 500, 1230000); // top
    push_msg(dut, MSG_ADD, false, 1, 101, 400, 1230500);
    push_msg(dut, MSG_CXL, false, 1, 100, 500, 0); // all 500 gone
    check(dut->best_ask_price, 1230500, "rescan moved 2nd best up");
    check(dut->best_ask_shares, 400, "2nd best's shares");

    push_msg(dut, MSG_CXL, false, 1, 100, 1, 0); // ref 100 is gone now
    check(dut->miss_count, 1, "X on a removed order is a miss");

    dut->final();
    delete dut;
}

// EDGE: the lowest and highest prices ITCH allows
// $0.0001 = 1 and $200,000.0000 = 2,000,000,000 (spec: max Price(4) is 200,000.0000)
// they sit right next to the empty markers (bid 0, ask 0xFFFFFFFF), so they must not get mixed up
static void test_price_extremes(){
    std::printf("TEST16: lowest legal bid and highest legal ask don't look like an empty book\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 100, 10, 1); // $0.0001
    push_msg(dut, MSG_ADD, false, 1, 101, 20, 2000000000); // $200,000.0000
    check(dut->best_bid_price, 1, "bid at $0.0001");
    check(dut->best_bid_shares, 10, "its shares");
    check(dut->best_ask_price, 2000000000, "ask at $200,000");
    check(dut->best_ask_shares, 20, "its shares");

    push_msg(dut, MSG_DEL, false, 1, 100, 0, 0);
    push_msg(dut, MSG_DEL, false, 1, 101, 0, 0);
    check(dut->best_bid_price, BID_EMPTY, "bid back to empty");
    check(dut->best_ask_price, ASK_EMPTY, "ask back to empty");
    check(dut->miss_count, 0, "no misses");

    dut->final();
    delete dut;
}

// EDGE: order ref 0 and the all-1s ref (both XOR-fold to bucket 0, so they share a bucket too)
// 0 must not get treated like an empty slot
static void test_order_ref_extremes(){
    std::printf("TEST17: order ref 0 and 0xFFFFFFFFFFFFFFFF work like any other ref\n");
    Vorder_book *dut = fresh_dut();

    const uint64_t REF_MIN = 0;
    const uint64_t REF_MAX = 0xFFFFFFFFFFFFFFFFULL; // 64b max

    push_msg(dut, MSG_ADD, true, 1, REF_MIN, 300, 1230000);
    push_msg(dut, MSG_ADD, true, 1, REF_MAX, 200, 1230000);
    check(dut->best_bid_shares, 500, "both landed at the level");

    push_msg(dut, MSG_EXC, false, 1, REF_MAX, 50, 0);
    check(dut->best_bid_shares, 450, "partial on the max ref");
    push_msg(dut, MSG_DEL, false, 1, REF_MIN, 0, 0);
    check(dut->best_bid_shares, 150, "ref 0 found and deleted");
    check(dut->miss_count, 0, "neither ref missed");

    dut->final();
    delete dut;
}

// EDGE: msgs for other symbols get dropped, even ones with the SAME order ref as ours
// (order refs are unique across the day, but the book shouldn't depend on that)
static void test_other_symbols_ignored(){
    std::printf("TEST18: other stock_locates never touch the book\n");
    Vorder_book *dut = fresh_dut(); // tracking locate 1

    push_msg(dut, MSG_ADD, true, 1, 100, 500, 1230000);
    push_msg(dut, MSG_ADD, true, 2, 200, 900, 1240000); // locate 2, would be the new best if counted
    check(dut->best_bid_price, 1230000, "locate 2 add ignored");

    push_msg(dut, MSG_EXC, false, 2, 100, 100, 0); // same ref as ours, wrong symbol
    push_msg(dut, MSG_CXL, false, 2, 100, 100, 0);
    push_msg(dut, MSG_DEL, false, 2, 100, 0, 0);
    push_replace(dut, 2, 100, 300, 1, 1250000, true);
    check(dut->best_bid_price, 1230000, "locate 2 U didn't move our order");
    check(dut->best_bid_shares, 500, "our order untouched by locate 2 E/X/D/U");
    check(dut->miss_count, 0, "dropped msgs aren't misses");

    dut->final();
    delete dut;
}

// EDGE: a full bucket frees a slot on delete, and the next add gets it
static void test_bucket_slot_reuse(){
    std::printf("TEST19: a slot freed in a full bucket gets reused\n");
    Vorder_book *dut = fresh_dut();

    // same refs as TEST13, all hash to bucket 7 (1024 apart, so the low 10 bits match)
    uint64_t refs[5] = {7, 1031, 2055, 3079, 4103};
    for (int i = 0; i < 4; i++){
        push_msg(dut, MSG_ADD, true, 1, refs[i], 100, 1230000 - i);
    }
    push_msg(dut, MSG_ADD, true, 1, refs[4], 987, 1231000);
    check(dut->overflow_count, 1, "5th overflows a full bucket");

    push_msg(dut, MSG_DEL, false, 1, refs[1], 0, 0); // frees a slot
    push_msg(dut, MSG_ADD, true, 1, refs[4], 987, 1231000); // try the 5th again
    check(dut->overflow_count, 1, "no new overflow, it took the freed slot");
    check(dut->best_bid_price, 1231000, "5th order is the new best bid");
    check(dut->best_bid_shares, 987, "with its shares");
    check(dut->miss_count, 0, "no misses");

    dut->final();
    delete dut;
}

// EDGE: chip one order down to its very last share, then take that
static void test_down_to_last_share(){
    std::printf("TEST20: order chipped down to 1 share by E and X, then the last share goes\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, false, 1, 100, 500, 1230000);
    push_msg(dut, MSG_EXC, false, 1, 100, 200, 0); // 300 left
    push_msg(dut, MSG_CXL, false, 1, 100, 299, 0); // 1 left
    check(dut->best_ask_price, 1230000, "still there with 1 share");
    check(dut->best_ask_shares, 1, "exactly 1 share left");

    push_msg(dut, MSG_EXC, false, 1, 100, 1, 0); // last one
    check(dut->best_ask_price, ASK_EMPTY, "order and level gone");
    push_msg(dut, MSG_EXC, false, 1, 100, 1, 0);
    check(dut->miss_count, 1, "anything after that is a miss");

    dut->final();
    delete dut;
}

// EDGE: an execute bigger than what's left just clears the order (same as the model)
static void test_over_execute(){
    std::printf("TEST21: executing more shares than are left clears the order\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, false, 1, 100, 500, 1230000);
    push_msg(dut, MSG_ADD, false, 1, 101, 100, 1230000);
    push_msg(dut, MSG_EXC, false, 1, 100, 600, 0); // 600 > 500
    check(dut->best_ask_price, 1230000, "level survives (ref 101 is still there)");
    check(dut->best_ask_shares, 100, "only ref 101's 100 left, not 500-600 wrapped around");

    dut->final();
    delete dut;
}

// EDGE: 4 orders stacked at 1 price, removed one at a time by every removal type
// the level has to survive until the very last order goes
static void test_level_survives_until_last(){
    std::printf("TEST22: a level stays until its last order is gone (via X, E, D, then E)\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 1, 100, 1230000);
    push_msg(dut, MSG_ADD, true, 1, 2, 200, 1230000);
    push_msg(dut, MSG_ADD, true, 1, 3, 300, 1230000);
    push_msg(dut, MSG_ADD, true, 1, 4, 400, 1230000);
    push_msg(dut, MSG_ADD, true, 1, 5, 50, 1229000); // next level down
    check(dut->best_bid_shares, 1000, "100+200+300+400");

    push_msg(dut, MSG_CXL, false, 1, 1, 100, 0); // X takes all of ref 1
    check(dut->best_bid_shares, 900, "after X");
    push_msg(dut, MSG_EXC, false, 1, 2, 200, 0); // E fills all of ref 2
    check(dut->best_bid_shares, 700, "after E");
    push_msg(dut, MSG_DEL, false, 1, 3, 0, 0);
    check(dut->best_bid_shares, 400, "after D");
    check(dut->best_bid_price, 1230000, "level still best with 1 order left");

    push_msg(dut, MSG_EXC, false, 1, 4, 400, 0); // last order at the level
    check(dut->best_bid_price, 1229000, "level gone, next one promoted");
    check(dut->best_bid_shares, 50, "next level's shares");
    check(dut->miss_count, 0, "no misses");

    dut->final();
    delete dut;
}

// U = the old order leaves, a new ref comes in on the same side at a new price + size
// it's the only msg that changes 2 orders, so it has to look like 1 change from the outside

static void test_rpl_moves_order(){
    std::printf("TEST23: U moves the best bid to a new price and size under a new ref\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 100, 500, 1230000);
    push_msg(dut, MSG_ADD, true, 1, 101, 200, 1229000);

    // ref 100 is alone at the top, so the delete half empties the top level (rescan) before the add half
    int pulses = push_replace(dut, 1, 100, 900, 300, 1230500, false); // stale side says sell, it's a bid
    check(pulses, 1, "published exactly once (not after each half)");
    check(dut->best_bid_price, 1230500, "bid moved up to the new price");
    check(dut->best_bid_shares, 300, "U's shares are the new total");
    check(dut->best_ask_price, ASK_EMPTY, "still a bid (side came from the old order, not the stale bit)");

    push_msg(dut, MSG_EXC, false, 1, 100, 1, 0);
    check(dut->miss_count, 1, "old ref is gone");
    push_msg(dut, MSG_DEL, false, 1, 900, 0, 0);
    check(dut->miss_count, 1, "new ref is live");
    check(dut->best_bid_price, 1229000, "back to the only order left");
    check(dut->best_bid_shares, 200, "its shares");

    dut->final();
    delete dut;
}

static void test_rpl_to_worse_price(){
    std::printf("TEST24: U moves the best ask away, the next level becomes the top\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, false, 1, 100, 400, 1230000);
    push_msg(dut, MSG_ADD, false, 1, 101, 100, 1230100);

    int pulses = push_replace(dut, 1, 100, 200, 400, 1230200, true);
    check(pulses, 1, "published exactly once");
    check(dut->best_ask_price, 1230100, "next level promoted");
    check(dut->best_ask_shares, 100, "its shares");

    push_msg(dut, MSG_DEL, false, 1, 101, 0, 0);
    check(dut->best_ask_price, 1230200, "the replaced order is next in line");
    check(dut->best_ask_shares, 400, "with its shares");
    check(dut->miss_count, 0, "no misses");

    dut->final();
    delete dut;
}

static void test_rpl_same_price(){
    std::printf("TEST25: U at the same price keeps the level and swaps in the new size\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 100, 500, 1230000);
    push_msg(dut, MSG_ADD, true, 1, 101, 200, 1230000);

    push_replace(dut, 1, 100, 102, 800, 1230000, false); // size up (that's why it loses its place in line)
    check(dut->best_bid_price, 1230000, "same level");
    check(dut->best_bid_shares, 1000, "200 + 800");

    // the level has to still count 2 orders (101 and 102), so 1 delete can't empty it
    push_msg(dut, MSG_DEL, false, 1, 101, 0, 0);
    check(dut->best_bid_shares, 800, "only the replaced order left");
    push_msg(dut, MSG_DEL, false, 1, 102, 0, 0);
    check(dut->best_bid_price, BID_EMPTY, "level gone with its last order");
    check(dut->miss_count, 0, "no misses");

    dut->final();
    delete dut;
}

static void test_rpl_unknown_ref(){
    std::printf("TEST26: U on a ref that isn't in the book is 1 miss and adds nothing\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 100, 500, 1230000);

    int pulses = push_replace(dut, 1, 555, 600, 900, 1240000, true); // would be the new best bid if it went in
    check(pulses, 0, "nothing published");
    check(dut->miss_count, 1, "1 miss");
    check(dut->best_bid_price, 1230000, "new order did NOT get added");
    check(dut->best_bid_shares, 500, "book unchanged");

    push_msg(dut, MSG_DEL, false, 1, 600, 0, 0);
    check(dut->miss_count, 2, "new ref never went in");

    dut->final();
    delete dut;
}

static void test_rpl_only_order_on_side(){
    std::printf("TEST27: U of the only bid never publishes the empty side in between\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, false, 1, 100, 300, 1230500);
    push_msg(dut, MSG_ADD, true, 1, 101, 100, 1230000);

    // between the halves the bid side is empty (best_bid = 0), but book_valid must stay LO there
    int pulses = push_replace(dut, 1, 101, 102, 150, 1230100, false);
    check(pulses, 1, "only the finished U got published");
    check(dut->best_bid_price, 1230100, "bid at its new price");
    check(dut->best_bid_shares, 150, "with the new size");
    check(dut->best_ask_price, 1230500, "ask side untouched");

    dut->final();
    delete dut;
}

static void test_rpl_buckets(){
    std::printf("TEST28: U into its own full bucket reuses the freed slot, into another full bucket overflows\n");
    Vorder_book *dut = fresh_dut();

    // refs < 2^16 hash to their low 10 bits, so these 4 fill bucket 7 and these 4 fill bucket 8
    uint64_t bucket7[4] = {7, 1031, 2055, 3079};
    uint64_t bucket8[4] = {8, 1032, 2056, 3080};
    for (int i = 0; i < 4; i++){
        push_msg(dut, MSG_ADD, true, 1, bucket7[i], 100, 1230000 - i);
        push_msg(dut, MSG_ADD, true, 1, bucket8[i], 100, 1220000 - i);
    }

    // 4103 is bucket 7 too: the delete half frees 1031's slot right before the add half needs one
    push_replace(dut, 1, 1031, 4103, 250, 1230000, false);
    check(dut->overflow_count, 0, "took the slot its old order just freed");
    check(dut->best_bid_shares, 350, "ref 7's 100 + the new 250");

    // 4104 is bucket 8, which is full: old order still leaves, new one can't go in
    int pulses = push_replace(dut, 1, 7, 4104, 100, 1235000, false);
    check(dut->overflow_count, 1, "add half overflowed");
    check(pulses, 1, "still published (the delete half changed the book)");
    check(dut->best_bid_price, 1230000, "the overflowed order isn't the top");
    check(dut->best_bid_shares, 250, "only 4103 left at the level");

    push_msg(dut, MSG_DEL, false, 1, 4104, 0, 0);
    check(dut->miss_count, 1, "4104 never went in");

    dut->final();
    delete dut;
}

// H/h decide whether we're allowed to trade the symbol (fail closed: blocked until an H 'T')

static void test_trading_state_from_h(){
    std::printf("TEST29: blocked out of reset, only H 'T' allows trading, H never touches the book\n");
    Vorder_book *dut = fresh_dut();

    check(dut->symbol_trading, 0, "blocked out of reset (no H seen yet)");
    check(dut->trading_state, 0, "no state yet");

    push_msg(dut, MSG_ADD, true, 1, 100, 500, 1230000);
    int pulses = push_trading_action(dut, 1, 'T');
    check(dut->symbol_trading, 1, "H 'T' = trading");
    check(dut->trading_state, 'T', "state readable");
    check(pulses, 0, "H doesn't publish a book update");
    check(dut->best_bid_shares, 500, "book untouched");

    push_trading_action(dut, 1, 'H');
    check(dut->symbol_trading, 0, "H 'H' = halted");
    push_trading_action(dut, 1, 'T');
    push_trading_action(dut, 1, 'P');
    check(dut->symbol_trading, 0, "H 'P' = paused (LULD)");
    push_trading_action(dut, 1, 'T');
    push_trading_action(dut, 1, 'Q');
    check(dut->symbol_trading, 0, "H 'Q' = quotation only, still no trading");
    push_trading_action(dut, 1, 'Z');
    check(dut->symbol_trading, 0, "a state byte the spec doesn't have = blocked");
    push_trading_action(dut, 1, 'T');
    check(dut->symbol_trading, 1, "back to trading");

    dut->final();
    delete dut;
}

static void test_op_halt_from_h(){
    std::printf("TEST30: only a Nasdaq ('Q') operational halt blocks us, BX/PSX ones don't\n");
    Vorder_book *dut = fresh_dut();

    push_trading_action(dut, 1, 'T');
    push_op_halt(dut, 1, 'B', 'H'); // BX halted it
    push_op_halt(dut, 1, 'X', 'H'); // PSX halted it
    check(dut->symbol_trading, 1, "other market centers' halts don't stop us on Nasdaq");

    push_op_halt(dut, 1, 'Q', 'H');
    check(dut->nasdaq_op_halted, 1, "Nasdaq op halt seen");
    check(dut->symbol_trading, 0, "Nasdaq halted it");

    // the 2 kinds of halt are separate: an H 'T' doesn't lift a Nasdaq op halt
    push_trading_action(dut, 1, 'T');
    check(dut->symbol_trading, 0, "H 'T' alone doesn't undo the op halt");
    push_op_halt(dut, 1, 'B', 'T');
    check(dut->symbol_trading, 0, "BX resuming doesn't undo Nasdaq's halt");

    push_op_halt(dut, 1, 'Q', 'T');
    check(dut->symbol_trading, 1, "Nasdaq lifted it");

    dut->final();
    delete dut;
}

static void test_halts_other_symbols_ignored(){
    std::printf("TEST31: H/h for other stock_locates never change our trading state\n");
    Vorder_book *dut = fresh_dut(); // tracking locate 1

    push_trading_action(dut, 2, 'T');
    check(dut->symbol_trading, 0, "locate 2's H 'T' doesn't unblock us");

    push_trading_action(dut, 1, 'T');
    push_trading_action(dut, 2, 'H');
    push_op_halt(dut, 2, 'Q', 'H');
    check(dut->symbol_trading, 1, "locate 2's halts don't block us");

    dut->final();
    delete dut;
}

static void test_symbol_switch_blocks(){
    std::printf("TEST32: software switching symbols blocks trading until the new symbol's H\n");
    Vorder_book *dut = fresh_dut();

    push_trading_action(dut, 1, 'T');
    check(dut->symbol_trading, 1, "trading locate 1");

    dut->cfg_stock_locate = 2;
    tick(dut);
    tick(dut);
    check(dut->symbol_trading, 0, "locate 1's 'T' says nothing about locate 2");

    push_trading_action(dut, 2, 'T');
    check(dut->symbol_trading, 1, "locate 2's own H 'T'");

    dut->final();
    delete dut;
}

// the ladder's last slot (1023) has to be in the rescan too
// 1007600 and 1220589 both hash to slot 1023 (16b halves XORed, low 10 bits)
static void test_rescan_finds_last_slot(){
    std::printf("TEST33: a rescan finds a level sitting in the ladder's last slot\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, false, 1, 100, 829, 1002500); // best ask, slot 11
    push_msg(dut, MSG_ADD, false, 1, 101, 342, 1007600); // next ask, slot 1023
    push_msg(dut, MSG_DEL, false, 1, 100, 0, 0); // top empties, rescan
    check(dut->best_ask_price, 1007600, "ask in slot 1023 promoted");
    check(dut->best_ask_shares, 342, "its shares");
    push_msg(dut, MSG_DEL, false, 1, 101, 0, 0); // clear the asks so the bids below can't cross them

    push_msg(dut, MSG_ADD, true, 1, 200, 500, 1225000); // best bid
    push_msg(dut, MSG_ADD, true, 1, 201, 77, 1220589); // next bid, slot 1023
    push_msg(dut, MSG_DEL, false, 1, 200, 0, 0);
    check(dut->best_bid_price, 1220589, "bid in slot 1023 promoted");
    check(dut->best_bid_shares, 77, "its shares");

    dut->final();
    delete dut;
}

static void test_lifecycle(){
    std::printf("TEST11: full book lifecycle\n");
    Vorder_book *dut = fresh_dut();

    push_msg(dut, MSG_ADD, true, 1, 1, 100, 1229000);
    push_msg(dut, MSG_ADD, true, 1, 2, 200, 1229500);
    push_msg(dut, MSG_ADD, true, 1, 3, 300, 1230000);
    push_msg(dut, MSG_ADD, false, 1, 4, 400, 1230500);
    push_msg(dut, MSG_ADD, false, 1, 5, 500, 1231000);
    check(dut->best_bid_price, 1230000, "best bid price");
    check(dut->best_bid_shares, 300, "best bid shares");
    check(dut->best_ask_price, 1230500, "best ask price");
    check(dut->best_ask_shares, 400, "best ask shares");
    check(dut->best_ask_price - dut->best_bid_price, 500, "spread (to be consumed by signal engine)");
    push_msg(dut, MSG_EXC, false, 1, 4, 400, 0);
    check(dut->best_bid_price, 1230000, "ask activity shouldn't affect bid");
    check(dut->best_ask_price, 1231000, "ask moved up");
    check(dut->best_ask_shares, 500, "new ask shares");
    check(dut->overflow_count, 0, "no overflowing");
    check(dut->miss_count, 0, "no misses");
    check(dut->level_collision_count, 0, "no L2 level collisions");

    dut->final();
    delete dut;
}

static std::vector<uint64_t> live_refs;
static std::map<uint64_t, bool> ref_is_buy; // side of every ref (a U has to keep its new price on the same side)
static constexpr size_t MAX_LIVE_ORDERS = 300;
static constexpr uint32_t PRICE_BASE = 1230000;
static constexpr uint32_t PRICE_SPAN = 20;

// bids below PRICE_BASE, asks at or above it, so the random book never crosses
static uint32_t random_price(bool is_buy){
    if (is_buy){
        return PRICE_BASE - 1 - (rand() % PRICE_SPAN);
    }
    return PRICE_BASE + (rand() % PRICE_SPAN);
}

static void forget_ref(uint64_t ref){
    for (size_t i = 0; i < live_refs.size(); i++){
        if (ref == live_refs[i]){
            // swap w last elem + pop
            live_refs[i] = live_refs.back();
            live_refs.pop_back();
            return;
        }
    }
}

static void test_random_against_model(int num_msgs, unsigned int seed){
    std::printf("TEST12: %d random messages against the reference model\n", num_msgs);
    Vorder_book *dut = fresh_dut();
    BookModel model;

    live_refs.clear();
    ref_is_buy.clear();
    uint64_t next_ref = 1;
    srand(seed);

    for (int n = 0; n < num_msgs; n++){
        int x = rand() % 100;

        if (live_refs.empty()){
            x = 0;
        }
        else if (x<45 && live_refs.size()>=MAX_LIVE_ORDERS){
            x = 45;
        }

        if (x < 45){
            // add
            uint64_t ref = next_ref++;
            bool is_buy = (rand() % 2) == 0;
            
            uint32_t shares = 1 + (rand() % 1000);
            uint32_t price = random_price(is_buy); // make sure we dont randomly pick a crossed market

            push_msg(dut, MSG_ADD, is_buy, 1, ref, shares, price);
            model.add_order(ref, is_buy, price, shares);
            live_refs.push_back(ref);
            ref_is_buy[ref] = is_buy;
        }
        else if (x < 57){
            // exc
            size_t i = rand() % live_refs.size();
            uint64_t ref = live_refs[i];
            uint32_t shares = 1 + (rand() % 1200);
            
            push_msg(dut, MSG_EXC, false, 1, ref, shares, 0);
            model.exc_order(ref, shares);

            if (!model.has_order(ref)){
                forget_ref(ref);
            }
        }
        else if (x < 69){
            // cxl (partial cancel, sometimes asks for more than is left which just clears the order)
            size_t i = rand() % live_refs.size();
            uint64_t ref = live_refs[i];
            uint32_t shares = 1 + (rand() % 1200);

            push_msg(dut, MSG_CXL, false, 1, ref, shares, 0);
            model.cancel_order(ref, shares);

            if (!model.has_order(ref)){
                forget_ref(ref);
            }
        }
        else if (x < 80){
            // rpl (cancel-replace: new ref, new price + size, same side)
            size_t i = rand() % live_refs.size();
            uint64_t ref = live_refs[i];
            uint64_t new_ref = next_ref++;
            bool is_buy = ref_is_buy[ref];
            uint32_t shares = 1 + (rand() % 1000);
            uint32_t price = random_price(is_buy);

            push_replace(dut, 1, ref, new_ref, shares, price, (rand() % 2) == 0);
            model.replace_order(ref, new_ref, price, shares);

            live_refs[i] = new_ref; // same slot, the order just goes by the new ref now
            ref_is_buy[new_ref] = is_buy;
        }
        else if (x < 90){
            // del
            size_t i = rand() % live_refs.size();
            uint64_t ref = live_refs[i];

            push_msg(dut, MSG_DEL, false, 1, ref, 0, 0);
            model.del_order(ref);
             
            forget_ref(ref);
        }
        else{
            // miss
            uint64_t ref = next_ref + 1000000; // go far past anything issued
            int kind = rand() % 4;
            if (kind == 0){
                push_msg(dut, MSG_EXC, false, 1, ref, 100, 0);
                model.exc_order(ref, 100);
            }
            else if (kind == 1){
                push_msg(dut, MSG_CXL, false, 1, ref, 100, 0);
                model.cancel_order(ref, 100);
            }
            else if (kind == 2){
                push_msg(dut, MSG_DEL, false, 1, ref, 0, 0);
                model.del_order(ref);
            }
            else{
                // new price would be the best bid possible, so if the RTL wrongly adds it the top shows it
                uint64_t new_ref = next_ref++;
                push_replace(dut, 1, ref, new_ref, 100, PRICE_BASE - 1, true);
                model.replace_order(ref, new_ref, PRICE_BASE - 1, 100);
            }
        }

        if (dut->best_bid_price != model.best_bid_price() || 
            dut->best_bid_shares != model.best_bid_shares() ||
            dut->best_ask_price != model.best_ask_price() ||
            dut->best_ask_shares != model.best_ask_shares() ||
            dut->miss_count != model.get_miss_count()){
            
            std::printf("FAILED! message: %d (seed: %u)\n", n, seed);
            std::printf("Bid price; dut %u; model %u\n",
                        (unsigned)dut->best_bid_price, (unsigned)model.best_bid_price());
            std::printf("Bid shares; dut %u; model %u\n",
                        (unsigned)dut->best_bid_shares, (unsigned)model.best_bid_shares());
            std::printf("Ask price; dut %u; model %u\n",
                        (unsigned)dut->best_ask_price, (unsigned)model.best_ask_price());
            std::printf("Ask shares; dut %u; model %u\n",
                        (unsigned)dut->best_ask_shares, (unsigned)model.best_ask_shares());
            std::printf("miss_count; dut %u; model %u\n",
                        (unsigned)dut->miss_count, (unsigned)model.get_miss_count());
            std::printf("live orders %zu; overflow_count %u\n",
                        live_refs.size(), (unsigned)dut->overflow_count);
            

            failures++;
            break;
        }
    }
    check(dut->overflow_count, 0, "no overflow");
    check(dut->level_collision_count, 0, "no level collisions");

    //
    dut->final();
    delete dut;
}

static void test_bucket_overflow(){
    std::printf("TEST13: 5th order in 1 bucket should cause overflow\n");
    Vorder_book *dut = fresh_dut();

    uint64_t refs[5] = {7, 1031, 2055, 3079, 4103};
    for (int i = 0; i < 4; i++){
        push_msg(dut, MSG_ADD, true, 1, refs[i], 100, 1230000 - i);
    }

    check(dut->overflow_count, 0, "4/4 fit");
    check(dut->best_bid_price, 1230000, "best bid from the first add");
    check(dut->best_bid_shares, 100, "corresponding shares");
    push_msg(dut, MSG_ADD, true, 1, refs[4], 987, 1231000);
    check(dut->overflow_count, 1, "5th order overflows");
    check(dut->best_bid_price, 1230000, "dropped order doesn't reach book");
    push_msg(dut, MSG_DEL, false, 1, refs[0], 0, 0);
    check(dut->miss_count, 0, "first order findable after overflow");
    check(dut->best_bid_price, 1229999, "2nd best got promoted");

    dut->final();
    delete dut;
}

int main(int argc, char **argv){
    Verilated::commandArgs(argc, argv);

    test_reset();
    test_add_sets_topofbook();
    test_add_only_improves_topofbook();
    test_add_aggregates_same_price();
    test_exc_partial_reduces_shares();
    test_exc_full_empties_level_and_rescans();
    test_exc_unknown_ref_counts_miss();
    test_del_removes_order_and_rescans();
    test_del_leaves_level_existing();
    test_del_unknown_ref_counts_miss();
    test_lifecycle();
    test_random_against_model(10000, 12345);
    test_random_against_model(10000, 11111);
    test_random_against_model(10000, 9);
    test_bucket_overflow();
    test_cxl_partial_keeps_order();
    test_cxl_full_empties_level_and_rescans();
    test_price_extremes();
    test_order_ref_extremes();
    test_other_symbols_ignored();
    test_bucket_slot_reuse();
    test_down_to_last_share();
    test_over_execute();
    test_level_survives_until_last();
    test_rpl_moves_order();
    test_rpl_to_worse_price();
    test_rpl_same_price();
    test_rpl_unknown_ref();
    test_rpl_only_order_on_side();
    test_rpl_buckets();
    test_trading_state_from_h();
    test_op_halt_from_h();
    test_halts_other_symbols_ignored();
    test_symbol_switch_blocks();
    test_rescan_finds_last_slot();

    std::printf("\n%s (Failures: %d)\n",
                failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}