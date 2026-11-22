// replays 1 symbol from a real Nasdaq day through the whole RTL pipeline (top) and checks the RTL's
// top of book, miss count and trading state against BookModel after every packet
// (1 msg per packet by default, so that's after every message)
/*
rm -rf obj_dir
verilator --cc --exe --build -j 0 --assert +define+SIM --public-flat-rw --top-module top rtl/msg_pkg.sv rtl/skid_buffer.sv rtl/async_fifo.sv rtl/moldudp_deframer.sv rtl/itch_parser.sv rtl/order_book.sv rtl/trade_signal.sv rtl/top_csr.sv rtl/top.sv tb/replay/replay_rtl.cpp tb/replay/itch_file_reader.cpp -LDFLAGS -lz -o replay_rtl
./obj_dir/replay_rtl ~/data/itch/12302019.NASDAQ_ITCH50.gz <TICKER> [msgs per packet]
*/
// --public-flat-rw is only so this tb can see the FSM states inside top and know when the pipeline is empty

#include "Vtop.h"
#include "Vtop___024root.h" // internal signals (from --public-flat-rw)
#include <verilated.h>
#include "itch_file_reader.hpp"
#include "itch_to_model.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <vector>

// core clk like tb_top, but the DMA side runs faster than the core here
// so the async FIFO fills and backpressures the whole time (the board's DMA is slower, this is the harder case)
static constexpr uint64_t CORE_PERIOD = 14;
static constexpr uint64_t DMA_PERIOD = 6;
static constexpr int RESET_CYCLES = 10;

// the pipeline counts as empty once it's been idle this many core cycles in a row
// (more than the async FIFO's pointer sync, so a byte still crossing can't look like "empty")
static constexpr int IDLE_CYCLES = 8;

// a rescan is ~1024 cycles, so anything past this means the pipeline is stuck
static constexpr int STUCK_CYCLES = 100000;

// a real UDP packet tops out ~1500 B, so 50 msgs of at most 2+50 B each stays under it
static constexpr int MAX_MSGS_PER_PACKET = 50;

// progress line every 100k of our symbol's messages
static constexpr uint64_t PROGRESS_EVERY = 100000;

static constexpr uint32_t REG_STOCK_LOCATE = 0x14;

static uint64_t sim_time = 0;

// CLOCKS
// jumps straight to the next clock edge instead of evaluating every time unit
// rising edge at every multiple of the period, falling edge halfway between

static uint64_t next_edge_time(){
    uint64_t core_next = (sim_time / (CORE_PERIOD/2) + 1) * (CORE_PERIOD/2);
    uint64_t dma_next = (sim_time / (DMA_PERIOD/2) + 1) * (DMA_PERIOD/2);
    return (core_next < dma_next) ? core_next : dma_next;
}

static void step(Vtop *dut, uint64_t t){
    REQUIRES(dut != nullptr);
    REQUIRES(t > sim_time);
    //

    sim_time = t;
    dut->core_clk = (sim_time % CORE_PERIOD) < (CORE_PERIOD/2);
    dut->dma_clk = (sim_time % DMA_PERIOD) < (DMA_PERIOD/2);
    dut->eval();
}

static bool core_rises_at(uint64_t t){
    return (t % CORE_PERIOD) == 0;
}
static bool dma_rises_at(uint64_t t){
    return (t % DMA_PERIOD) == 0;
}

// steps to the next core rising edge
static void core_cycle(Vtop *dut){
    REQUIRES(dut != nullptr);
    //

    while (true){
        uint64_t t = next_edge_time();
        step(dut, t);
        if (core_rises_at(t)){
            return;
        }
    }
}

static void reset(Vtop *dut){
    REQUIRES(dut != nullptr);
    //

    dut->core_rst_n = 0;
    dut->dma_rst_n = 0;
    dut->s_axis_tvalid = 0;
    dut->s_axis_tlast = 0;
    dut->s_axis_tdata = 0;
    dut->s_axi_awvalid = 0;
    dut->s_axi_wvalid = 0;
    dut->s_axi_bready = 0;
    dut->s_axi_arvalid = 0;
    dut->s_axi_rready = 0;
    dut->s_axi_wstrb = 0xF;

    for (int i = 0; i < RESET_CYCLES; i++){
        core_cycle(dut);
    }
    dut->core_rst_n = 1;
    dut->dma_rst_n = 1;
    for (int i = 0; i < RESET_CYCLES; i++){
        core_cycle(dut);
    }
}

// AXI-Lite write; the handshakes get sampled right BEFORE the core rising edge (that's when the slave sees them)
static void axi_write(Vtop *dut, uint32_t addr, uint32_t data){
    REQUIRES(dut != nullptr);
    //

    dut->s_axi_awaddr = addr & 0xFF;
    dut->s_axi_awvalid = 1;
    dut->s_axi_wdata = data;
    dut->s_axi_wvalid = 1;
    dut->s_axi_bready = 1;

    for (int i = 0; i < STUCK_CYCLES; i++){
        uint64_t t = next_edge_time();
        bool aw_fire = false;
        bool w_fire = false;
        bool b_fire = false;
        if (core_rises_at(t)){
            aw_fire = dut->s_axi_awvalid && dut->s_axi_awready;
            w_fire = dut->s_axi_wvalid && dut->s_axi_wready;
            b_fire = dut->s_axi_bvalid && dut->s_axi_bready;
        }
        step(dut, t);

        if (aw_fire) dut->s_axi_awvalid = 0;
        if (w_fire) dut->s_axi_wvalid = 0;
        if (b_fire){
            dut->s_axi_bready = 0;
            return;
        }
    }
    throw std::runtime_error("CSR write never got a response");
}

// pushes 1 packet's bytes on the DMA side; a byte only moves on when tvalid && tready right before a DMA rising edge
static void push_packet(Vtop *dut, const std::vector<uint8_t> &packet){
    REQUIRES(dut != nullptr);
    REQUIRES(!packet.empty());
    //

    size_t i = 0;
    dut->s_axis_tvalid = 1;
    dut->s_axis_tdata = packet[0];
    dut->s_axis_tlast = (packet.size() == 1);

    uint64_t stalled = 0;
    while (i < packet.size()){
        uint64_t t = next_edge_time();
        bool accepted = dma_rises_at(t) && dut->s_axis_tready;
        step(dut, t);

        if (accepted){
            i++;
            stalled = 0;
            if (i < packet.size()){
                dut->s_axis_tdata = packet[i];
                dut->s_axis_tlast = (i == packet.size() - 1);
            }
        }
        else if (++stalled > (uint64_t)STUCK_CYCLES * CORE_PERIOD){
            throw std::runtime_error("DMA side stuck, the FIFO never took a byte");
        }
    }
    dut->s_axis_tvalid = 0;
    dut->s_axis_tlast = 0;
}

// nothing left anywhere: FIFO empty, deframer not offering a byte, parser between msgs with nothing to hand off, book IDLE
static bool pipeline_empty(Vtop *dut){
    Vtop___024root *r = dut->rootp;
    return r->top__DOT__fifo_empty
        && !r->top__DOT__deframer_tvalid
        && r->top__DOT__u_parser__DOT__state == 0 // READ_TYPE
        && !r->top__DOT__u_parser__DOT__fsm_tvalid
        && !r->top__DOT__parser_tvalid
        && r->top__DOT__u_book__DOT__state == 0; // IDLE
}

static void wait_until_empty(Vtop *dut){
    REQUIRES(dut != nullptr);
    //

    int idle_run = 0;
    for (int i = 0; i < STUCK_CYCLES; i++){
        core_cycle(dut);
        idle_run = pipeline_empty(dut) ? idle_run + 1 : 0;
        if (idle_run == IDLE_CYCLES){
            return;
        }
    }
    throw std::runtime_error("pipeline never went idle");
}

// MoldUDP64 packet: 10 B session + 8 B sequence + 2 B count, then 2 B length + msg for each msg
static void build_packet(std::vector<uint8_t> &packet, uint64_t sequence, const std::vector<std::vector<uint8_t>> &msgs){
    packet.assign(MOLD_HEADER_LEN, 0);
    build_mold_header(&packet[0], sequence, (uint16_t)msgs.size());
    for (const std::vector<uint8_t> &m : msgs){
        size_t n = packet.size();
        packet.resize(n + 2 + m.size());
        build_mold_msg(&packet[n], m.data(), (uint16_t)m.size());
    }
}

// R (stock directory): stock symbol @11, 8 chars right-padded with spaces
static bool is_ticker(const std::vector<uint8_t> &msg, const char *padded_ticker){
    return std::memcmp(&msg[11], padded_ticker, 8) == 0;
}

// timestamp @5 is ns since midnight, printed as HH:MM:SS.microseconds
static void print_time(FILE *out, uint64_t ns){
    uint64_t s = ns / 1000000000ULL;
    std::fprintf(out, "%02llu:%02llu:%02llu.%06llu", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60),
        (unsigned long long)(s % 60), (unsigned long long)(ns / 1000 % 1000000));
}

// the RTL drops orders or loses levels once it's out of room, the first time each happened
struct CapacityEvent{
    bool seen = false;
    uint64_t msg_index = 0; // our symbol's message count when it showed up
    uint64_t timestamp = 0;
};

static void note_capacity(CapacityEvent &e, uint32_t count, uint64_t msg_index, uint64_t timestamp){
    if (!e.seen && count > 0){
        e.seen = true;
        e.msg_index = msg_index;
        e.timestamp = timestamp;
    }
}

int main(int argc, char **argv){
    Verilated::commandArgs(argc, argv);
    Verilated::fatalOnError(false); // an assertion failing gets reported with where in the day it happened

    if (argc < 3){
        std::fprintf(stderr, "usage: %s <NASDAQ_ITCH50.gz> <TICKER> [msgs per packet, 1-%d]\n", argv[0], MAX_MSGS_PER_PACKET);
        return 2;
    }
    int per_packet = (argc > 3) ? std::atoi(argv[3]) : 1;
    if (per_packet < 1 || per_packet > MAX_MSGS_PER_PACKET){
        std::fprintf(stderr, "msgs per packet has to be 1-%d\n", MAX_MSGS_PER_PACKET);
        return 2;
    }

    // R messages pad the symbol to 8 chars with spaces
    char ticker[9];
    std::memset(ticker, ' ', 8);
    ticker[8] = '\0';
    std::memcpy(ticker, argv[2], std::min<size_t>(std::strlen(argv[2]), 8));

    Vtop *dut = new Vtop;
    reset(dut);

    BookModel model;
    bool found = false;
    uint16_t locate = 0;

    // what the RTL's trading state should be (from H/h, same rules as the spec)
    uint8_t h_state = 0; // no H yet
    bool nasdaq_op_halted = false;

    std::vector<std::vector<uint8_t>> pending; // our msgs waiting to go out in the next packet
    std::vector<uint8_t> packet;
    uint64_t sequence = 1;
    uint64_t packets = 0;
    uint64_t sent = 0;
    uint64_t sent_by_type[256] = {};
    uint64_t last_timestamp = 0;

    CapacityEvent first_overflow;
    CapacityEvent first_collision;
    bool mismatch = false;
    bool assertion = false;

    try{
        ItchFileReader rd(argv[1]);
        std::vector<uint8_t> msg;

        // sends what's pending, waits for the pipeline to drain, then RTL vs model
        // returns false on the first mismatch
        auto flush_and_check = [&]() -> bool{
            if (pending.empty()){
                return true;
            }
            build_packet(packet, sequence, pending);
            push_packet(dut, packet);
            wait_until_empty(dut);
            sequence += pending.size();
            packets++;
            pending.clear();

            if (Verilated::gotError()){
                assertion = true;
                return false;
            }

            note_capacity(first_overflow, dut->overflow_count, sent, last_timestamp);
            note_capacity(first_collision, dut->level_collision_count, sent, last_timestamp);

            bool trading = (h_state == 'T') && !nasdaq_op_halted;
            if (dut->best_bid_price == model.best_bid_price()
                && dut->best_bid_shares == model.best_bid_shares()
                && dut->best_ask_price == model.best_ask_price()
                && dut->best_ask_shares == model.best_ask_shares()
                && dut->miss_count == model.get_miss_count()
                && dut->symbol_trading == trading){
                return true;
            }

            std::printf("\nMISMATCH after msg %llu of %s (", (unsigned long long)sent, argv[2]);
            print_time(stdout, last_timestamp);
            std::printf(")\n");
            std::printf("bid price: rtl %u model %u\n", (unsigned)dut->best_bid_price, model.best_bid_price());
            std::printf("bid shares: rtl %u model %u\n", (unsigned)dut->best_bid_shares, model.best_bid_shares());
            std::printf("ask price: rtl %u model %u\n", (unsigned)dut->best_ask_price, model.best_ask_price());
            std::printf("ask shares: rtl %u model %u\n", (unsigned)dut->best_ask_shares, model.best_ask_shares());
            std::printf("misses: rtl %u model %u\n", (unsigned)dut->miss_count, model.get_miss_count());
            std::printf("trading: rtl %u expected %u\n", (unsigned)dut->symbol_trading, (unsigned)trading);
            mismatch = true;
            return false;
        };

        while (rd.next(msg)){
            uint8_t type = msg[0];

            // the R for our symbol comes before any of its orders, that's where its locate for the day comes from
            if (!found){
                if (type == 'R' && is_ticker(msg, ticker)){
                    locate = read_u16_bigendian(&msg[1]); //@1
                    axi_write(dut, REG_STOCK_LOCATE, locate);
                    found = true;
                    std::printf("%s is locate %u\n", argv[2], (unsigned)locate);
                }
                continue;
            }
            if (read_u16_bigendian(&msg[1]) != locate){ //@1
                continue;
            }

            // only what the RTL acts on goes out (the parser drops everything else anyway)
            if (is_book_changing(type)){
                apply_to_book(model, msg);
            }
            else if (type == 'H'){
                h_state = msg[19]; //@19 trading state
            }
            else if (type == 'h'){
                if (msg[19] == 'Q'){ //@19 market code, Q = Nasdaq
                    nasdaq_op_halted = (msg[20] != 'T'); //@20 action
                }
            }
            else{
                continue;
            }

            pending.push_back(msg);
            sent++;
            sent_by_type[type]++;
            last_timestamp = read_u48_bigendian(&msg[5]); //@5

            if ((int)pending.size() == per_packet && !flush_and_check()){
                break;
            }
            if (sent % PROGRESS_EVERY == 0){
                std::fprintf(stderr, "  %llu msgs sent (", (unsigned long long)sent);
                print_time(stderr, last_timestamp);
                std::fprintf(stderr, ")\n");
            }
        }
        if (!mismatch && !assertion){
            flush_and_check(); // whatever's left at the close
        }
    }
    catch (const std::exception &e){
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }

    if (!found){
        std::printf("no R message for %s in this file\n", argv[2]);
        return 1;
    }

    std::printf("\nsent %llu msgs for %s in %llu packets:", (unsigned long long)sent, argv[2], (unsigned long long)packets);
    for (char t : {'A', 'F', 'E', 'C', 'X', 'D', 'U', 'H', 'h'}){
        std::printf(" %c=%llu", t, (unsigned long long)sent_by_type[(uint8_t)t]);
    }
    std::printf("\n");

    std::printf("bucket overflows: %u", (unsigned)dut->overflow_count);
    if (first_overflow.seen){
        std::printf(" (first one by msg %llu, ", (unsigned long long)first_overflow.msg_index);
        print_time(stdout, first_overflow.timestamp);
        std::printf(")");
    }
    std::printf("\nladder collisions: %u", (unsigned)dut->level_collision_count);
    if (first_collision.seen){
        std::printf(" (first one by msg %llu, ", (unsigned long long)first_collision.msg_index);
        print_time(stdout, first_collision.timestamp);
        std::printf(")");
    }
    std::printf("\n");

    dut->final();
    delete dut;

    // a mismatch after the RTL ran out of room is expected (it dropped something the model kept)
    // a mismatch with no capacity event before it is a real RTL bug
    bool capacity = first_overflow.seen || first_collision.seen;
    if (assertion && !capacity){
        std::printf("FAILED (an assertion fired and the RTL never ran out of room, so this is a bug)\n");
        return 1;
    }
    if (assertion){
        std::printf("STOPPED (an assertion fired after the RTL ran out of room, see above)\n");
        return 3;
    }
    if (mismatch && !capacity){
        std::printf("FAILED (RTL disagrees with the model and it never ran out of room, so this is a bug)\n");
        return 1;
    }
    if (mismatch){
        std::printf("STOPPED (RTL ran out of room first, so the mismatch is the capacity limit, not a bug)\n");
        return 3;
    }
    if (capacity){
        std::printf("PASSED on top of book, but the RTL ran out of room (dropped orders/levels never reached the top)\n");
        return 0;
    }
    std::printf("PASSED (RTL matched the model after every packet, all day)\n");
    return 0;
}
