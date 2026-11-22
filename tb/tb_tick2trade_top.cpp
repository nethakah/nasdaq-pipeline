/*
rm -rf obj_dir
verilator --cc --exe --build -j 0 --assert +define+SIM --top-module tick2trade_top rtl/msg_pkg.sv rtl/skid_buffer.sv rtl/async_fifo.sv rtl/moldudp_deframer.sv rtl/itch_parser.sv rtl/order_book.sv rtl/trade_signal.sv rtl/tick2trade_csr.sv rtl/tick2trade_top.sv tb/tb_tick2trade_top.cpp
./obj_dir/Vtick2trade_top
*/

#include "Vtick2trade_top.h"
#include <verilated.h>
#include <cstdio>
#include <cstdint>
#include "contracts.hpp"
#include "itch_messages.hpp"
#include <cstdlib>

static int failures = 0;
static constexpr int DMA_PERIOD = 20;
static constexpr int CORE_PERIOD = 13;
static constexpr int RESET_UNITS = DMA_PERIOD * 5; // scale off slower clk
static constexpr uint32_t BID_EMPTY = 0;
static constexpr uint32_t ASK_EMPTY = 0xFFFFFFFF;
static constexpr uint32_t REG_CONTROL = 0x00;
static constexpr uint32_t REG_TRIGGER_PRICE = 0x04;
static constexpr uint32_t REG_ORDER_SHARES = 0x08;
static constexpr uint32_t REG_SPREAD_MAX = 0x0C;
static constexpr uint32_t REG_SIZE_MIN = 0x10;
static constexpr uint32_t REG_STOCK_LOCATE = 0x14;
static constexpr uint32_t REG_FIRE_LATENCY = 0x4C;
static constexpr uint32_t REG_LATENCY_MIN = 0x50;
static constexpr uint32_t REG_LATENCY_MAX = 0x54;
static constexpr uint32_t REG_PACKET_LATENCY = 0x58;

static constexpr uint32_t REG_BEST_BID_PRICE = 0x20;
static constexpr uint32_t REG_BEST_ASK_PRICE = 0x28;
static constexpr uint32_t REG_PACKET_COUNT = 0x38;
static constexpr uint32_t REG_MISS_COUNT = 0x40;

static uint64_t sim_time = 0;

static void check(uint64_t actual, uint64_t expected, const char *name){
    REQUIRES(name != nullptr);
    //
    
    if (actual != expected){
        std::printf("FAIL %-34s got: 0x%llx, expected: 0x%llx\n",
                    name, (unsigned long long)actual, (unsigned long long)expected);
        failures++;
    }
}

static void tick(Vtick2trade_top *dut){
    REQUIRES(dut != nullptr);
    //

    sim_time++;
    int dma_phase = sim_time % DMA_PERIOD;
    int core_phase = sim_time % CORE_PERIOD;

    if (dma_phase == 0) dut->dma_clk = 1; // rising
    else if (dma_phase == DMA_PERIOD/2) dut->dma_clk = 0; // falling
    if (core_phase == 0) dut->core_clk = 1; // rising
    else if (core_phase == CORE_PERIOD/2) dut->core_clk = 0; // falling
    // period/2 because 2 edges per period

    dut->eval();
}

static bool dma_rising(){
    return (sim_time % DMA_PERIOD) == 0;
}
static bool core_rising(){
    return (sim_time % CORE_PERIOD) == 0;
}

static void reset(Vtick2trade_top *dut){
    REQUIRES(dut != nullptr);
    //

    dut->dma_clk = 0;
    dut->core_clk = 0;
    dut->dma_rst_n = 0;
    dut->core_rst_n = 0;
    dut->s_axis_tdata = 0;
    dut->s_axis_tvalid = 0;
    dut->s_axis_tlast = 0;
    
    dut->s_axi_awvalid = 0;
    dut->s_axi_wvalid = 0;
    dut->s_axi_bready = 0;
    dut->s_axi_arvalid = 0;
    dut->s_axi_rready = 0;
    dut->s_axi_wstrb = 0xF;
    
    for (int i = 0; i < RESET_UNITS; i++){
        tick(dut);
    }
    dut->dma_rst_n = 1;
    dut->core_rst_n = 1;
    for (int i = 0; i < RESET_UNITS; i++){
        tick(dut);
    }

    //
    ENSURES(dut->best_bid_price == BID_EMPTY);
    ENSURES(dut->best_ask_price == ASK_EMPTY);
}

static Vtick2trade_top *fresh_dut(){
    Vtick2trade_top *dut = new Vtick2trade_top;
    reset(dut);
    return dut;
}

static void axi_write(
    Vtick2trade_top *dut,
    uint32_t addr,
    uint32_t data,
    int max_time = CORE_PERIOD*100
){
    REQUIRES(dut != nullptr);
    REQUIRES(dut->core_rst_n==1);
    //

    dut->s_axi_awaddr = addr & 0xFF;
    dut->s_axi_awvalid = 1;
    dut->s_axi_wdata = data;
    dut->s_axi_wstrb = 0xF;
    dut->s_axi_wvalid = 1;
    dut->s_axi_bready = 1;

    bool aw_done = false;
    bool w_done = false;
    bool b_done = false;

    for (int i = 0; i < max_time; i++){
        tick(dut);
        if (core_rising()){
            if (!aw_done && dut->s_axi_awready){
                aw_done = true;
            }
            if (!w_done && dut->s_axi_wready){
                w_done = true;
            }
            if (dut->s_axi_bvalid){
                b_done = true;
            }
        }

        if (aw_done) dut->s_axi_awvalid = 0;
        if (w_done) dut->s_axi_wvalid = 0;
        if (b_done) break;
    }
    dut->s_axi_awvalid = 0;
    dut->s_axi_wvalid = 0;

    for (int i = 0; i < CORE_PERIOD*2; i++){
        tick(dut);
    }
    dut->s_axi_bready = 0;
    
    if (!b_done){
        std::printf("FAILED: tried to write to CSR (0x%02x) but never got a response\n", addr);
        failures++;
    }
}

static uint32_t axi_read(
    Vtick2trade_top *dut,
    uint32_t addr,
    int max_time = CORE_PERIOD*100
){
    REQUIRES(dut != nullptr);
    REQUIRES(dut->core_rst_n == 1);
    //

    dut->s_axi_araddr = addr & 0xFF;
    dut->s_axi_arvalid = 1;
    dut->s_axi_rready = 1;
    uint32_t sample = 0;
    bool ar_done = false;
    bool r_done = false;

    for (int i = 0; i < max_time; i++){
        tick(dut);
        if (core_rising()){
            if (!ar_done && dut->s_axi_arready){
                ar_done = true;
            }
            if (dut->s_axi_rvalid){
                sample = dut->s_axi_rdata;
                r_done = true;
            }
        }

        if (ar_done) dut->s_axi_arvalid = 0;
        if (r_done) break;
    }

    dut->s_axi_arvalid = 0;
    for (int i = 0; i < CORE_PERIOD*2; i++){
        tick(dut);
    }
    dut->s_axi_rready = 0;

    if (!r_done){
        std::printf("FAILED: tried to read from CSR (0x%02x) but it never returned data\n", addr);
        failures++;
    }
    
    return sample;
}

static void preload(
    Vtick2trade_top *dut,
    bool armed,
    bool side,
    uint32_t trigger_price,
    uint32_t order_shares,
    uint32_t spread_max,
    uint32_t size_min
){
    REQUIRES(dut != nullptr);
    //

    axi_write(dut, REG_STOCK_LOCATE, 1);
    axi_write(dut, REG_TRIGGER_PRICE, trigger_price);
    axi_write(dut, REG_ORDER_SHARES, order_shares);
    axi_write(dut, REG_SPREAD_MAX, spread_max);
    axi_write(dut, REG_SIZE_MIN, size_min);
    axi_write(dut, REG_CONTROL, (armed? 0x1 : 0x0) | (side? 0x2 : 0x0));
}

static void push_byte(
    Vtick2trade_top *dut,
    uint8_t b,
    bool last,
    int max_time = DMA_PERIOD*50
){
    REQUIRES(dut != nullptr);
    REQUIRES(dut->dma_rst_n == 1);
    //

    dut->s_axis_tdata = b;
    dut->s_axis_tlast = last ? 1 : 0;
    dut->s_axis_tvalid = 1;

    for (int i = 0; i < max_time; i++){
        tick(dut);
        if (dma_rising() && dut->s_axis_tready){
            tick(dut);
            dut->s_axis_tvalid = 0;
            return;
        }
    }

    std::printf("FAILED: the byte was not accepted by the DMA interface");
    failures++;
}

static void push_packet(
    Vtick2trade_top *dut,
    const uint8_t *packet,
    size_t len,
    int drain_time = CORE_PERIOD*400
){
    REQUIRES(dut != nullptr);
    REQUIRES(packet != nullptr);
    REQUIRES(len > 0);
    //

    size_t byte_index = 0;
    dut->s_axis_tvalid = 1; // hold tvalid high for entire packet to make sure no gaps works
    dut->s_axis_tdata = packet[0];
    dut->s_axis_tlast = (len==1);

    for (int t = 0; t < DMA_PERIOD*50*(int)len; t++){
        tick(dut);
        if (dma_rising() && dut->s_axis_tready){
            byte_index++;
            if (byte_index >= len){
                break;
            }
            dut->s_axis_tdata = packet[byte_index];
            dut->s_axis_tlast = (byte_index == len - 1);
        }
    }
    dut->s_axis_tvalid = 0;
    if (len > byte_index){
        std::printf("FAILED: was only able to accept %zu / %zu B\n", byte_index, len);
        failures++;
    }
    for (int t = 0; t < drain_time; t++){
        tick(dut);
    }

    // for (int i = 0; i < len; i++){
    //     push_byte(dut, packet[i], i==len-1);
    // }
    // dut->s_axis_tvalid = 0;

    // for (int i = 0; i < drain_time; i++){
    //     tick(dut);
    // }
}

// reset to baselines works
static void test_reset(){
    std::printf("TEST1: resets to empty book\n");
    Vtick2trade_top *dut = fresh_dut();

    check(dut->best_bid_price, BID_EMPTY, "bid at baseline");
    check(dut->best_ask_price, ASK_EMPTY, "ask at baseline");
    check(dut->packet_count, 0, "packet_count = 0");
    check(dut->gap_count, 0, "gap_count = 0");
    check(dut->fire_count, 0, "fire_count = 0");
    check(dut->miss_count, 0, "miss_count = 0");
    check(dut->overflow_count, 0, "overflow_count = 0");

    dut->final();
    delete dut;
}

// check MoldUDP64 packet with 2 Add orders goes in and book shows both sides
static void test_end2end_book(){
    std::printf("TEST2: on raw packet input, order book state outputs accordingly\n");
    Vtick2trade_top *dut = fresh_dut();
    axi_write(dut, REG_STOCK_LOCATE, 1);

    OrderAdd bid;
    bid.stock_locate  = 1;
    bid.tracking_num  = 1;
    bid.timestamp = 1000;
    bid.order_ref_num = 100;
    bid.is_buy = true;
    bid.shares = 500;
    bid.stock = "AAPL";
    bid.price = 0x01020304;

    OrderAdd ask;
    ask.stock_locate = 1;
    ask.tracking_num = 2;
    ask.timestamp = 2000;
    ask.order_ref_num = 200;
    ask.is_buy = false;
    ask.shares = 300;
    ask.stock = "AAPL";
    ask.price = 0x05060708;

    uint8_t itch_bid[ORDER_ADD_LEN];
    uint8_t itch_ask[ORDER_ADD_LEN];
    build_order_add(itch_bid, bid);
    build_order_add(itch_ask, ask);

    uint8_t packet[128];
    size_t n = 0;
    build_mold_header(&packet[0], 1000, 2);
    n = MOLD_HEADER_LEN;
    n += build_mold_msg(&packet[n], itch_bid, ORDER_ADD_LEN);
    n += build_mold_msg(&packet[n], itch_ask, ORDER_ADD_LEN);

    push_packet(dut, packet, n);
    check(dut->best_bid_price, 0x01020304, "best bid from raw bytes");
    check(dut->best_ask_price, 0x05060708, "best ask from raw bytes");
    check(dut->best_bid_shares, 500, "best bid shares");
    check(dut->best_ask_shares, 300, "best ask shares");
    check(dut->packet_count, 1, "packet_count = 1");
    check(dut->gap_count, 0, "no gaps");
    check(dut->sequence_num, 1000, "seq num");
    check(dut->spread, 0x04040404, "spread computed");
    check(dut->miss_count, 0, "no misses");
    check(dut->overflow_count, 0, "no overflow");
    check(dut->level_collision_count, 0, "no L2 level collisions");

    dut->final();
    delete dut;
}

// check raw packets in one side and fired order on other side
static void test_end2end_fire(){
    std::printf("TEST3: on raw packet input, order fires accordingly\n");
    Vtick2trade_top *dut = fresh_dut();

    preload(dut, true, true, 1230500, 100, 1000, 50);
    // buy 100 shares if ask is 123.05 or better

    OrderAdd bid;
    bid.stock_locate  = 1;
    bid.tracking_num  = 1;
    bid.timestamp = 1000;
    bid.order_ref_num = 100;
    bid.is_buy = true;
    bid.shares = 500;
    bid.stock = "AAPL";
    bid.price = 1230000;

    OrderAdd ask;
    ask.stock_locate = 1;
    ask.tracking_num = 2;
    ask.timestamp = 2000;
    ask.order_ref_num = 200;
    ask.is_buy = false;
    ask.shares = 300;
    ask.stock = "AAPL";
    ask.price = 1230500;

    uint8_t itch_bid[ORDER_ADD_LEN];
    uint8_t itch_ask[ORDER_ADD_LEN];
    build_order_add(itch_bid, bid);
    build_order_add(itch_ask, ask);

    uint8_t packet[128];
    size_t n = 0;
    build_mold_header(&packet[0], 1000, 2);
    n = MOLD_HEADER_LEN;
    n += build_mold_msg(&packet[n], itch_bid, ORDER_ADD_LEN);
    n += build_mold_msg(&packet[n], itch_ask, ORDER_ADD_LEN);

    push_packet(dut, packet, n);
    check(dut->fire_count, 1, "1 fire");
    check(dut->order_side, 1, "buy side");
    check(dut->order_price, 1230500, "order has our limit");
    check(dut->order_shares, 100, "order has preloaded size");

    uint32_t decision = axi_read(dut, REG_FIRE_LATENCY);
    uint32_t ingest = axi_read(dut, REG_PACKET_LATENCY);
    std::printf("\ndecision latency: %u cycles (%.1f ns) (pipeline reaction)\n", 
                (unsigned)decision, decision*3.476);
    std::printf("\ningest latency: %u cycles (%.1f ns) (including 96-byte ingest at testbench's DMA rate)\n", 
                (unsigned)ingest, ingest*3.476);

    dut->final();
    delete dut;
}

// push 2 packets w sequence jumps
static void test_gap_detection(){
    std::printf("TEST4: sequence gap detected from end to end of pipeline\n");
    Vtick2trade_top *dut = fresh_dut();
    axi_write(dut, REG_STOCK_LOCATE, 1);

    OrderDelete del;
    del.stock_locate = 1;
    del.tracking_num = 1;
    del.timestamp = 1000;
    del.order_ref_num = 987;

    uint8_t itch_del[ORDER_DELETE_LEN];
    build_order_delete(itch_del, del);

    uint64_t seq[2] = {1000, 1005};
    for (int i = 0; i < 2; i++){
        uint8_t packet[128];
        size_t n = 0;
        build_mold_header(&packet[0], seq[i], 2);
        n = MOLD_HEADER_LEN;
        n += build_mold_msg(&packet[n], itch_del, ORDER_DELETE_LEN);
        n += build_mold_msg(&packet[n], itch_del, ORDER_DELETE_LEN);
        push_packet(dut, packet, n);
    }

    check(dut->packet_count, 2, "2 packets");
    check(dut->gap_count, 3, "1005 - 1002 = 3 messages lost");
    check(dut->miss_count, 4, "4 misses on unknown orders");

    dut->final();
    delete dut;
}

static void test_kill_switch(){
    std::printf("TEST5: kill switch blocks fires immediately end to end of pipeline\n");
    Vtick2trade_top *dut = fresh_dut();

    preload(dut, false, true, 1230500, 100, 1000, 50); // disarmed

    OrderAdd bid;
    bid.stock_locate  = 1;
    bid.tracking_num  = 1;
    bid.timestamp = 1000;
    bid.order_ref_num = 100;
    bid.is_buy = true;
    bid.shares = 500;
    bid.stock = "AAPL";
    bid.price = 1230000;

    OrderAdd ask;
    ask.stock_locate = 1;
    ask.tracking_num = 2;
    ask.timestamp = 2000;
    ask.order_ref_num = 200;
    ask.is_buy = false;
    ask.shares = 300;
    ask.stock = "AAPL";
    ask.price = 1230500;

    uint8_t itch_bid[ORDER_ADD_LEN];
    uint8_t itch_ask[ORDER_ADD_LEN];
    build_order_add(itch_bid, bid);
    build_order_add(itch_ask, ask);

    uint8_t packet[128];
    size_t n = 0;
    build_mold_header(&packet[0], 1000, 2);
    n = MOLD_HEADER_LEN;
    n += build_mold_msg(&packet[n], itch_bid, ORDER_ADD_LEN);
    n += build_mold_msg(&packet[n], itch_ask, ORDER_ADD_LEN);

    push_packet(dut, packet, n);
    check(dut->best_ask_price, 1230500, "book updated still");
    check(dut->fire_count, 0, "nothing fired");

    dut->final();
    delete dut;
}

// appends one skip-type msg (random body) to a packet, returns bytes written
static size_t add_skip_msg(uint8_t *dest, char type){
    REQUIRES(dest != nullptr);
    //

    for (size_t i = 0; i < NUM_SKIP_TYPES; i++){
        if (SKIP_TYPES[i].type == type){
            uint8_t msg[ITCH_MAX_MSG_LEN];
            build_skip_msg(msg, SKIP_TYPES[i]);
            return build_mold_msg(dest, msg, (uint16_t)SKIP_TYPES[i].len);
        }
    }
    ASSERT(false); // not a skip type
    return 0;
}

// packets that look like real data: non-book msgs mixed in, another symbol, and all 7 book types
// (A, F, E, C, X, D in the 1st packet, U in the 2nd), with an E and a U that each empty a top level
static void test_real_mix_packet(){
    std::printf("TEST6: packets with a real-day mix of msg types end to end\n");
    Vtick2trade_top *dut = fresh_dut();
    axi_write(dut, REG_STOCK_LOCATE, 1);
    srand(4242); // skip msg bodies are random; fixed seed so a failure reproduces

    OrderAdd bid;
    bid.stock_locate = 1;
    bid.tracking_num = 1;
    bid.timestamp = 1000;
    bid.order_ref_num = 100;
    bid.is_buy = true;
    bid.shares = 500;
    bid.stock = "AAPL";
    bid.price = 1230000;

    OrderAdd bid2 = bid; // 2nd level down
    bid2.order_ref_num = 300;
    bid2.shares = 100;
    bid2.price = 1229900;

    OrderAdd ask = bid; // comes in as an F
    ask.order_ref_num = 200;
    ask.is_buy = false;
    ask.shares = 300;
    ask.price = 1230500;

    OrderAdd other = bid; // different symbol, would be the best bid if it counted
    other.stock_locate = 2;
    other.order_ref_num = 400;
    other.shares = 999;
    other.price = 1240000;

    OrderCancel cxl; // 500 -> 300 on the best bid
    cxl.stock_locate = 1;
    cxl.tracking_num = 3;
    cxl.timestamp = 3000;
    cxl.order_ref_num = 100;
    cxl.cancelled_shares = 200;

    OrderExecutedPrice exc_price; // 300 -> 200 on the ask, at a price that isn't the order's
    exc_price.stock_locate = 1;
    exc_price.tracking_num = 4;
    exc_price.timestamp = 4000;
    exc_price.order_ref_num = 200;
    exc_price.shares = 100;
    exc_price.match_num = 1;
    exc_price.printable = true;
    exc_price.exec_price = 1230400;

    OrderExecuted exc; // fills the rest of the best bid, so the top level empties and rescans
    exc.stock_locate = 1;
    exc.tracking_num = 5;
    exc.timestamp = 5000;
    exc.order_ref_num = 100;
    exc.shares = 300;
    exc.match_num = 2;

    OrderReplace rpl; // moves the ask (the F, 200 left after the C) to a new price and size under a new ref
    rpl.stock_locate = 1;
    rpl.tracking_num = 6;
    rpl.timestamp = 6000;
    rpl.orig_order_ref_num = 200;
    rpl.new_order_ref_num = 500;
    rpl.shares = 250;
    rpl.price = 1230300;

    uint8_t a_bid[ORDER_ADD_LEN];
    uint8_t a_bid2[ORDER_ADD_LEN];
    uint8_t f_ask[ORDER_ADD_MPID_LEN];
    uint8_t a_other[ORDER_ADD_LEN];
    uint8_t x_msg[ORDER_CANCEL_LEN];
    uint8_t c_msg[ORDER_EXECUTED_PRICE_LEN];
    uint8_t e_msg[ORDER_EXECUTED_LEN];
    uint8_t u_msg[ORDER_REPLACE_LEN];
    build_order_add(a_bid, bid);
    build_order_add(a_bid2, bid2);
    build_order_add_mpid(f_ask, ask, "GSCO");
    build_order_add(a_other, other);
    build_order_cancel(x_msg, cxl);
    build_order_executed_price(c_msg, exc_price);
    build_order_executed(e_msg, exc);
    build_order_replace(u_msg, rpl);

    // 512B is plenty: 20B header + 14 msgs, the longest being 2+50
    uint8_t packet[512];
    size_t n = MOLD_HEADER_LEN;
    n += add_skip_msg(&packet[n], 'S'); // system event
    n += add_skip_msg(&packet[n], 'R'); // stock directory
    n += build_mold_msg(&packet[n], a_bid, ORDER_ADD_LEN);
    n += add_skip_msg(&packet[n], 'H'); // trading action
    n += build_mold_msg(&packet[n], f_ask, ORDER_ADD_MPID_LEN);
    n += build_mold_msg(&packet[n], a_bid2, ORDER_ADD_LEN);
    n += build_mold_msg(&packet[n], a_other, ORDER_ADD_LEN);
    n += add_skip_msg(&packet[n], 'P'); // trade vs a hidden order
    n += build_mold_msg(&packet[n], x_msg, ORDER_CANCEL_LEN);
    n += add_skip_msg(&packet[n], 'I'); // NOII, the longest msg
    n += build_mold_msg(&packet[n], c_msg, ORDER_EXECUTED_PRICE_LEN);
    n += add_skip_msg(&packet[n], 'h'); // operational halt (lowercase h is its own type)
    n += build_mold_msg(&packet[n], e_msg, ORDER_EXECUTED_LEN);
    n += add_skip_msg(&packet[n], 'L'); // market participant position
    build_mold_header(&packet[0], 5000, 14); // 14 msgs total, book or not

    // the E triggers a ~1024 cycle rescan, so drain long enough for that
    push_packet(dut, packet, n, CORE_PERIOD * 2000);

    check(dut->best_bid_price, 1229900, "2nd level promoted after the top emptied");
    check(dut->best_bid_shares, 100, "its shares");
    check(dut->best_ask_price, 1230500, "F landed as the ask");
    check(dut->best_ask_shares, 200, "300 - 100 from the C");
    check(dut->miss_count, 0, "every E/C/X found its order");
    check(dut->overflow_count, 0, "no overflow");
    check(dut->packet_count, 1, "1 packet");
    check(dut->gap_count, 0, "no gaps");

    // 2nd packet picks up right where the 1st left off (seq 5000 + 14 msgs = 5014)
    uint8_t packet2[128]; // 20B header + (2+35)B U + (2+44)B P = 103B
    size_t n2 = MOLD_HEADER_LEN;
    n2 += build_mold_msg(&packet2[n2], u_msg, ORDER_REPLACE_LEN);
    n2 += add_skip_msg(&packet2[n2], 'P'); // trade vs a hidden order
    build_mold_header(&packet2[0], 5014, 2);

    // the U's delete half empties the top ask level (rescan), so same long drain
    push_packet(dut, packet2, n2, CORE_PERIOD * 2000);

    check(dut->best_ask_price, 1230300, "U moved the ask to its new price");
    check(dut->best_ask_shares, 250, "U's new total");
    check(dut->best_bid_price, 1229900, "bid side untouched by the U");
    check(dut->miss_count, 0, "U found its order");
    check(dut->packet_count, 2, "2 packets");
    check(dut->gap_count, 0, "no gap between the 2 packets");

    dut->final();
    delete dut;
}

int main(int argc, char **argv){
    Verilated::commandArgs(argc, argv);

    test_reset();
    test_end2end_book();
    test_end2end_fire();
    test_gap_detection();
    test_kill_switch();
    test_real_mix_packet();

    std::printf("\n%s (Failures: %d)\n",
                failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}