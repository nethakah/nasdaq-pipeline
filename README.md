# Complete Nasdaq Trading Pipeline (HFT) on FPGA
`SystemVerilog` · `Verilator/C++` · `Vivado 2024.1` · `PYNQ` · `Xilinx ZCU104 (XCZU7EV)`

This project is my own build for a Nasdaq TotalView-ITCH 5.0 market data pipeline. As per high-frequency trading (HFT), this is implemented completely on an FPGA board, with configurable thresholds (memory writing in C/C++) to change trading strategies.

Practically: MoldUDP64 packets in over AXI4-Stream, limit order book reconstructed on-chip, and a preloaded order fires when the book hits threshold conditions set via AXI-Lite.

| Metric | Value |
| :-- | :-- |
| Decision latency | 33.3 ns (8 cycles @ 240 MHz) |
| Clock, in-system | 240 MHz, +0.130 ns post-route |
| Clock, standalone | 288 MHz out-of-context |
| LUTs | 20,229 / 230,400 (8.78%) |
| Registers | 3,060 / 460,800 (0.66%) |
| BRAM / URAM / DSP | 0 / 0 / 0 |

Note: Decision latency, in this case, is measuring from the last byte of the ITCH message to `order_fire` asserting (the fabric timestamps itself and reports over AXI-Lite).

Note: The hardware numbers above are from the last bitstream build (`sw/overlay/`), which was before full message coverage, halt gating, and gap handling went in. They get re-measured on the next rebuild. In simulation the decision latency is still 8 cycles.

Overview of the datapath:
```
1. DDR4
2. AXI-DMA (100 MHz)
3. async_fifo (CDC)
4. moldudp_deframer       -> a sequence gap trips the kill switch in top_csr
5. itch_parser (240 MHz)
6. order_book             <- top_csr sets which symbol (stock_locate)
7. trade_signal           <- top_csr sets the thresholds and kill switch
```
The on-chip order book here is 3 levels of LUTRAM (see /build_logs/ to see how I came to this):
- L3 hashes `order_ref_num` into 1024 buckets 4 wide, compared in parallel to make lookup a fixed cycle count.
- L2 is a price ladder per side (buy/sell).
- L1 is best bid and ask.
- Buckets are 640 bits.

Note: This is a model for a single ticker symbol; the book filters for one configured `stock_locate` and drops the rest. A real Nasdaq day (8,906 symbols) peaks at 1.9M live orders across the whole market (~279 Mb), and the xczu7ev only has ~38 Mb of BRAM + URAM, so every full book on chip doesn't fit. The plan for multi-symbol is to keep only the orders in the top 10 levels on chip (158 to 474 orders even for the busiest symbols) and the rest in DRAM (see log-64).

## ITCH 5.0 coverage
All 23 message types have their lengths in the parser, since even a message that gets dropped has to be walked to stay aligned with the next one.

| Type | What the pipeline does with it |
| :-- | :-- |
| A, F | Add order (F's MPID is ignored) |
| E, C | Execute (C still shrinks the order at its resting price) |
| X | Partial cancel (the order keeps its place in line) |
| D | Delete |
| U | Replace: deletes the old ref and adds the new one on the same side, published as 1 update |
| H, h | Trading halts (see below) |
| The other 14 | Skipped by length |

Checked against a real day (`12302019.NASDAQ_ITCH50.gz`, 268.7M messages): the C++ reference model finds every order across all 263M book messages (0 misses).

## Fail closed
The trade signal only fires when everything says it's safe, and anything uncertain counts as a no:
- Nothing fires until an H 'T' (trading) arrives for the symbol. Nasdaq's spec says a symbol missing from the pre-open H spin should be treated as halted. H 'H' / 'P' / 'Q' and a Nasdaq operational halt (h with market 'Q') block it too.
- A MoldUDP64 sequence gap trips the kill switch, since the lost packets could have held anything for our symbol. Software has to re-arm (bit 2 of the control register says a gap did it).
- Switching symbols blocks trading until the new symbol's H.
- A U never publishes the half-done book between its delete and its add.

## Layout
```
rtl/
    msg_pkg.sv              shared types, hashes, all 23 ITCH message lengths
    skid_buffer.sv          AXI4-Stream pipeline register
    async_fifo.sv           the CDC, gray pointers + ASYNC_REG
    moldudp_deframer.sv     UDP envelope, sequence tracking, gap count
    itch_parser.sv          ITCH bytes to a struct, drops the types nothing uses
    order_book.sv           L3 hash buckets, L2 ladders, L1 top of book, halt state
    trade_signal.sv         fire conditions, kill switch, halt gate
    top_csr.sv              AXI-Lite register file
    top.sv                  wires it together
tb/
    book_model.hpp          golden reference model of the book (all 7 book types)
    itch_messages.hpp       message builders, shared with sw/
    tb_*.cpp                one Verilator testbench per module, tb_top.cpp is end to end
    replay/                 streams a real Nasdaq day file through the reference model
fpga/
    constraints/            OOC constraints and the block design CDC file
    scripts/                package_ip.tcl, create_bd.tcl, synth, impl
    results/                timing and utilization reports
sw/
    gen_itch.cpp            packet generator, runs on the board
    run.py                  PYNQ driver, programs the PL and reads counters
    deploy.sh               copies everything the board needs
    overlay/                the .bit and .hwh these numbers came from
build_logs/
    journal.md              what I tried and why
    bugs.md                 symptom / cause / fix
```

## Testing
- One Verilator testbench per module, plus `tb_top.cpp` end to end (raw MoldUDP64 packets in, fired order out).
- Randomized streams checked message by message against `book_model.hpp`, which is a different structure on purpose (sorted maps, no hashing, no capacity limit) so the model and the RTL can't share a bug.
- Backpressure comes in bursts up to ~1,100 cycles (like a real rescan), because 1-cycle stalls never fill the skid buffer.
- Edge cases: every field at max and at 0, garbage bytes, reset mid-message, refs 0 and 2^64-1, the lowest and highest legal prices, full buckets, and every U corner case.
- Mutation testing: I break the RTL on purpose and make sure a test fails for each break.
- SVA assertions on top of that (crossed book, AXI handshake rules, never firing into a halted symbol, ...).

## Quickstart
`REQUIRES: Verilator 5.x, Vivado 2024.1`

1. Lint:
```
    bash scripts/lint.sh
```
2. Simulate. The build command is at the top of every testbench, e.g. the whole pipeline end to end:
```
    rm -rf obj_dir && verilator --cc --exe --build -j 0 --assert +define+SIM --top-module top rtl/msg_pkg.sv rtl/skid_buffer.sv rtl/async_fifo.sv rtl/moldudp_deframer.sv rtl/itch_parser.sv rtl/order_book.sv rtl/trade_signal.sv rtl/top_csr.sv rtl/top.sv tb/tb_top.cpp && ./obj_dir/Vtop
```
3. Reference model over a real day (day files are on Nasdaq's public ITCH directory, ~3.5 GB compressed):
```
    g++ -std=c++17 -O2 -o tb/replay/replay_book tb/replay/replay_book.cpp tb/replay/itch_file_reader.cpp -lz
    ./tb/replay/replay_book <day>.NASDAQ_ITCH50.gz
```
4. Out-of-context synth and impl:
```
    bash scripts/synth.sh
    bash scripts/impl.sh
```
5. Package the RTL as IP (`fpga/ip/` is generated and gitignored):
```
    vivado -mode batch -source fpga/scripts/package_ip.tcl
```
6. In Vivado, with `ip_repo_paths` pointed at `fpga/ip/`:
```
    source fpga/scripts/create_bd.tcl
    add_files -fileset constrs_1 fpga/constraints/tick2trade_bd.xdc
```
- `create_bd.tcl` does NOT carry that constraint file; skip it and you get a bitstream with no CDC constraint.
7. Generate the bitstream, then:
```
    bash sw/deploy.sh user@board
```
- Board instructions in [sw/README.md](sw/README.md).

## Scope/Goals
- [x] MoldUDP64 deframer with sequence gap detection
- [x] ITCH 5.0 parser, all 23 message types (7 change the book, H/h gate trading, the rest get skipped)
- [x] Three-level order book in LUTRAM
- [x] Trade signal with preloaded order and kill switch
- [x] Halt gating (H/h) and gap handling, fail closed
- [x] Golden reference model and randomized regression
- [x] Reference model over a full real Nasdaq day
- [x] Running on ZCU104 over AXI-DMA
- [ ] Replay a real Nasdaq day through the RTL in Verilator
- [ ] L3 into URAM and a bigger ladder (a real day peaks at 37,068 orders and 4,227 levels a side in one symbol)
- [ ] Multi-symbol (top of book on chip, the rest in DRAM)
- [ ] ASIC flow with Genus/Innovus to find PPA in SRAM instead of LUTRAM
- [ ] Scatter-gather DMA for a real throughput number
- [ ] Ethernet front end, so it's wire-to-trade
- [ ] Order entry (OUCH over TCP) with inline risk checks

## License & Copyright
Copyright 2026 Nethaka Haldo

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
