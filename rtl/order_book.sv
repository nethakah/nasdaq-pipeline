/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off UNUSEDPARAM */

module order_book
    import msg_pkg::*;
(
    input logic clk,
    input logic rst_n,

    // slave - parsed msgs incoming from itch_parser.sv
    input msg_t s_axis_tdata,
    input logic s_axis_tvalid,
    output logic s_axis_tready,

    // which symbol this book tracks - written by software over axi4-lite
    input logic[15:0] cfg_stock_locate,

    // top of book (updated when a msg changes it)
    output logic[31:0] best_bid_price,
    output logic[31:0] best_bid_shares,
    output logic[31:0] best_ask_price,
    output logic[31:0] best_ask_shares,
    output logic book_valid,

    // whether we're allowed to trade this symbol (from H/h, never from the book itself)
    // FAIL CLOSED: blocked out of reset until an H 'T' says it's trading (spec: a symbol missing from
    // the pre-open H spin "should be treated as halted"), so a late start or a lost H can't let us trade
    output logic[7:0] trading_state, // last H state: 'T' trading / 'H' halted / 'P' paused / 'Q' quotation only / 0 = no H yet
    output logic nasdaq_op_halted, // h with market code 'Q': Nasdaq itself halted the symbol (where our orders go)
    output logic symbol_trading, // OK to trade = H said 'T' and Nasdaq isn't operationally halted

    // status updates
    output logic[31:0] overflow_count, // no free slot in bucket to Add
    output logic[31:0] miss_count, // executed/del referenced an order not in the book
    output logic [31:0] level_collision_count // 2 prices hashed to same level slot
);
    logic[31:0] top_bid_price;
    logic[31:0] top_bid_shares;
    logic[31:0] top_ask_price;
    logic[31:0] top_ask_shares;

    assign best_bid_price = top_bid_price;
    assign best_bid_shares = top_bid_shares;
    assign best_ask_price = top_ask_price;
    assign best_ask_shares = top_ask_shares;
    
    typedef enum logic[2:0]{
        IDLE = 3'd0,
        READ_BUCKET = 3'd1,
        FIND_ORDER = 3'd2, // if A, reads level here nad updates
        READ_LEVEL = 3'd3, // for E/D - read the level after learning the price
        UPDATE = 3'd4, // modify both L3 and L2
        RESCAN = 3'd5, // for when top level fully empties
        WALK = 3'd6 // top level emptied: check the next ticks out for the new top before falling back to RESCAN
    } book_state_enum;
    book_state_enum state;
    // ADD (4): IDLE -> READ_BUCKET -> FIND_ORDER -> UPDATE
    // E/D (5): IDLE -> READ_BUCKET -> FIND_ORDER -> READ_LEVEL -> UPDATE
    // U (8): the E/D path on the old ref, then READ_BUCKET -> FIND_ORDER -> UPDATE again as an ADD on the new ref
    //        (+ RESCAN between the 2 halves if the old order was the last one at the top level)
    // top level emptied: UPDATE -> WALK (1 cycle per tick to the next level) -> RESCAN only if nothing within 1024 ticks

    logic[BUCKET_BITS-1:0] book_mem[NUM_BUCKETS]; // L3 table - 1 read for whole bucket
    logic[LEVEL_BITS-1:0] bid_levels[NUM_LEVELS]; // L2 bids
    logic[LEVEL_BITS-1:0] ask_levels[NUM_LEVELS]; // L2 asks

    msg_t curr_msg;
    logic[BOOK_ADDR_WIDTH-1:0] curr_bucket; // hashed bucket index
    bucket_t bucket_data; // bucket we read

    // L2 registers (latched in FIND_ORDER)
    level_t level_data; // level we read
    logic[LEVEL_ADDR_WIDTH-1:0] curr_level; // hashed level index
    logic curr_is_buy; // which side's array to use
    logic[31:0] curr_price; // price of order being changed
    logic[31:0] shares_removed; // num of shares that leave this level
    logic order_left; // did order leave book entirely

    assign symbol_trading = (trading_state == "T") && !nasdaq_op_halted;

    // to catch software switching symbols (the old symbol's trading state means nothing for the new one)
    logic[15:0] prev_stock_locate;

    // U = delete the old ref, then add the new ref on the same side
    // HI between the 2 halves (old order is out, new one isn't in yet)
    logic rpl_pending;

    // flat mem needs next value assembled to be written as a word
    bucket_t bucket_next;
    level_t level_next;

    // store its price on the level to verify hash landed on right one
    logic level_match;
    assign level_match = level_data.valid && (level_data.price == curr_price);

    // for rescanning when top of book level empties
    logic[LEVEL_ADDR_WIDTH-1:0] rescan_index;
    logic rescan_is_buy;
    logic[31:0] rescan_best_price;
    logic[31:0] rescan_best_shares;

    // scratch wire for the rescan comparison (since we cannot cast and field select like level_t'(x).field)
    level_t rescan_level;
    always_comb begin
        if (rescan_is_buy) rescan_level = level_t'(bid_levels[rescan_index]);
        else rescan_level = level_t'(ask_levels[rescan_index]);
    end

    // live levels per side, so a side that just lost its last level goes empty right away instead of being searched
    logic[LEVEL_ADDR_WIDTH:0] bid_level_count; // 0 to NUM_LEVELS, so 1 more bit than a slot index
    logic[LEVEL_ADDR_WIDTH:0] ask_level_count;

    // WALK: the ladder is indexed by tick, so the next best level after the top empties is the next tick out that has one
    // (usually a tick or 2 away). the 1st slot holding exactly the price being checked is the new top, since every
    // price between it and the old top was just checked and isn't there.
    // no hit in 1024 ticks: if they were all on 1 side of $1 that was every slot once, so the best valid slot seen is the
    // new top (same answer as a RESCAN). only a walk that crossed $1 (2 tick sizes can share slots) needs the full RESCAN
    logic[31:0] walk_price; // price being checked this cycle
    logic[LEVEL_ADDR_WIDTH-1:0] walk_slot; // its slot, registered alongside it so the slot math isn't in front of the read
    logic[LEVEL_ADDR_WIDTH-1:0] walk_count; // ticks checked so far
    logic walk_crossed_dollar; // the walk went from penny ticks to sub-penny ticks or back
    level_t walk_level;
    always_comb begin
        if (rescan_is_buy) walk_level = level_t'(bid_levels[walk_slot]);
        else walk_level = level_t'(ask_levels[walk_slot]);
    end
    logic walk_found;
    assign walk_found = walk_level.valid && (walk_level.price == walk_price);

    // WALK and RESCAN both track the best valid level they've looked at
    level_t scan_level;
    assign scan_level = (state == WALK) ? walk_level : rescan_level;

    // best level so far INCLUDING this cycle's slot
    // (the last slot gets compared in the same cycle the result is written to the top of book)
    logic rescan_take; // this slot beats the best so far
    logic[31:0] rescan_next_price;
    logic[31:0] rescan_next_shares;
    always_comb begin
        if (rescan_is_buy) begin // higher bid replaces
            rescan_take = scan_level.valid && (scan_level.price > rescan_best_price);
        end
        else begin // lower ask replaces
            rescan_take = scan_level.valid && (scan_level.price < rescan_best_price);
        end
        rescan_next_price = rescan_take ? scan_level.price : rescan_best_price;
        rescan_next_shares = rescan_take ? scan_level.total_shares : rescan_best_shares;
    end

    // THE PARALLEL COMPARES
    logic[ENTRIES_PER_BUCKET-1:0] match_hit; // indices hold whether they are the order we want
    logic[ENTRIES_PER_BUCKET-1:0] free_hit; // indices hold whether they are empty
    always_comb begin
        for (int i = 0; i < ENTRIES_PER_BUCKET; i++) begin
            match_hit[i] = bucket_data[i].valid && (bucket_data[i].order_ref_num == curr_msg.order_ref_num);
            free_hit[i] = !bucket_data[i].valid;
        end
    end
    logic match_found;
    logic free_found;
    logic[$clog2(ENTRIES_PER_BUCKET)-1:0] match_index;
    logic[$clog2(ENTRIES_PER_BUCKET)-1:0] free_index;
    always_comb begin
        match_found = |match_hit; // OR every bit in match_hit
        free_found = |free_hit; // OR every bit in free_hit

        match_index = '0;
        free_index = '0;
        for (int i = 0; i < ENTRIES_PER_BUCKET; i++) begin
            if (match_hit[i]) begin
                match_index = $clog2(ENTRIES_PER_BUCKET)'(i); 
                // size cast to that width
            end
            if (free_hit[i]) begin
                free_index = $clog2(ENTRIES_PER_BUCKET)'(i); 
                // size cast to that width
            end
        end
    end

    // accept msg when idle (processing spans 4-5 cycles)
    assign s_axis_tready = (state == IDLE);

    // assemble next bucket bc flat mem takes whole-word writes only
    // start from what READ_BUCKET latched, apply change, and always_ff will do 1 assignment
    always_comb begin
        bucket_next = bucket_data;
        if (curr_msg.msg_type == MSG_ADD) begin
            bucket_next[free_index].valid = 1'b1;
            bucket_next[free_index].is_buy = curr_msg.is_buy;
            bucket_next[free_index].stock_locate = curr_msg.stock_locate;
            bucket_next[free_index].price = curr_msg.price;
            bucket_next[free_index].shares = curr_msg.shares;
            bucket_next[free_index].order_ref_num = curr_msg.order_ref_num;
        end
        else if (!order_left) begin
            bucket_next[match_index].shares = bucket_data[match_index].shares - shares_removed;
        end
        else begin
            bucket_next[match_index].valid = '0;
        end
    end
    always_comb begin
        level_next = level_data;
        if (curr_msg.msg_type == MSG_ADD) begin
            if (level_match) begin // level exists
                level_next.total_shares = level_data.total_shares + curr_msg.shares;
                level_next.order_count = level_data.order_count + 16'd1;
            end
            else begin // create fresh level
                level_next = '0;
                level_next.valid = 1'b1;
                level_next.price = curr_msg.price;
                level_next.total_shares = curr_msg.shares;
                level_next.order_count = 16'd1;
            end
        end
        else begin
            if (level_data.order_count == 16'd1 && order_left) begin
                // last order at this price
                level_next.valid = '0;
            end
            else begin
                level_next.total_shares = level_data.total_shares - shares_removed;
                if (order_left) begin
                    level_next.order_count = level_data.order_count - 16'd1;
                end
            end
        end
    end


    always_ff @(posedge clk) begin
        if (!rst_n) begin
            state <= IDLE;
            overflow_count <= '0;
            miss_count <= '0;
            level_collision_count <= '0;
            book_valid <= '0;
            rpl_pending <= '0;
            bid_level_count <= '0;
            ask_level_count <= '0;

            trading_state <= '0; // no H seen yet = not allowed to trade
            nasdaq_op_halted <= '0;
            prev_stock_locate <= cfg_stock_locate;

            top_bid_shares <= '0;
            top_ask_shares <= '0;
            top_bid_price <= '0; // new bids win by being higher so start at 0
            top_ask_price <= 32'hFFFFFFFF; // new asks win by being lower so we start at max 32b num
        end
        else begin
            book_valid <= '0; // 1cycle pulse

            // software switched symbols, so block again until the new symbol's H
            // (an H for the new symbol in this same cycle still counts)
            prev_stock_locate <= cfg_stock_locate;
            if (cfg_stock_locate != prev_stock_locate) begin
                trading_state <= '0;
                nasdaq_op_halted <= '0;
            end

            case (state)
                IDLE: begin
                    if (s_axis_tvalid && s_axis_tready && (s_axis_tdata.stock_locate==cfg_stock_locate)) begin
                        case (s_axis_tdata.msg_type)
                            // H/h never touch the book, they only flip whether we can trade, so they're done in this 1 cycle
                            MSG_ACT: begin
                                trading_state <= s_axis_tdata.trading_state;
                            end
                            MSG_OPH: begin
                                // only Nasdaq's own halt stops us (a BX or PSX halt doesn't stop trading on Nasdaq)
                                if (s_axis_tdata.market_code == "Q") begin
                                    nasdaq_op_halted <= (s_axis_tdata.op_halt_action != "T"); // anything but 'T' counts as halted
                                end
                            end

                            default: begin
                                curr_msg <= s_axis_tdata;
                                curr_bucket <= hash_order_ref(s_axis_tdata.order_ref_num);
                                state <= READ_BUCKET;
                            end
                        endcase
                    end
                end

                READ_BUCKET: begin
                    // sync read (address applied this cycle, data gets to bucket_data at next edge)
                    bucket_data <= bucket_t'(book_mem[curr_bucket]);
                    state <= FIND_ORDER;
                end

                FIND_ORDER: begin
                    // match_hit/free_hit settled off bucket_data, so searching is zero cycles
                    if (curr_msg.msg_type == MSG_ADD) begin
                        // Add has its price embedded so level address is known already
                        curr_price <= curr_msg.price;
                        curr_is_buy <= curr_msg.is_buy;
                        curr_level <= price_slot(curr_msg.price);
                        level_data <= curr_msg.is_buy ?
                                      level_t'(bid_levels[price_slot(curr_msg.price)]) : level_t'(ask_levels[price_slot(curr_msg.price)]);
                        state <= UPDATE;
                    end
                    else if (match_found) begin
                        // level read costs +1 cycle since E/D dont carry price
                        curr_price <= bucket_data[match_index].price;
                        curr_is_buy <= bucket_data[match_index].is_buy;
                        curr_level <= price_slot(bucket_data[match_index].price);

                        // if its DEL, a replace (the whole old order goes), or fully filled execute, then remove whole order
                        // (U's shares are the NEW order's total, so they can't be compared against the old order here)
                        if (curr_msg.msg_type == MSG_DEL || curr_msg.msg_type == MSG_RPL || curr_msg.shares >= bucket_data[match_index].shares) begin
                            shares_removed <= bucket_data[match_index].shares;
                            order_left <= 1'b1;
                        end
                        else begin
                            // partial fill
                            shares_removed <= curr_msg.shares;
                            order_left <= '0;
                        end
                        state <= READ_LEVEL;
                    end
                    else begin
                        // order not in the book or Add was lost in a seq gap
                        miss_count <= miss_count + 32'd1;
                        state <= IDLE;
                    end
                end 

                READ_LEVEL: begin
                    level_data <= curr_is_buy ? level_t'(bid_levels[curr_level]) : level_t'(ask_levels[curr_level]);
                    state <= UPDATE;
                end

                // every msg writes both things in 1 cycle here
                UPDATE: begin
                    // U goes back around to add the new order (after a RESCAN if the top level emptied)
                    state <= (curr_msg.msg_type == MSG_RPL) ? READ_BUCKET : IDLE;
                    case (curr_msg.msg_type)

                        MSG_ADD: begin
                            // find free slot in bucket + write order into it
                            // check every slot in parallel
                            if (free_found) begin
                                // L3 - always_comb assembled bucket_next
                                book_mem[curr_bucket] <= bucket_next;
                            
                                // L2 - add shares at price (build level and write 1 word)
                                if (level_match) begin // level exists
                                    if (curr_is_buy) begin
                                        bid_levels[curr_level] <= level_next;
                                    end
                                    else begin
                                        ask_levels[curr_level] <= level_next;
                                    end
                                    book_valid <= 1'b1;
                                end
                                else if (!level_data.valid) begin
                                    // empty slot so create the level
                                    if (curr_is_buy) begin
                                        bid_levels[curr_level] <= level_next;
                                        bid_level_count <= bid_level_count + 1'b1;
                                    end
                                    else begin
                                        ask_levels[curr_level] <= level_next;
                                        ask_level_count <= ask_level_count + 1'b1;
                                    end
                                    book_valid <= 1'b1;
                                end
                                else begin
                                    // slot has a different price, so ladder is undersized or hash is cooked
                                    level_collision_count <= level_collision_count + 32'd1;
                                end

                                // Top of Book
                                if (curr_is_buy) begin
                                    if (curr_msg.price == top_bid_price) begin
                                        top_bid_shares <= level_data.total_shares + curr_msg.shares;
                                    end
                                    else if (curr_msg.price > top_bid_price) begin
                                        top_bid_price <= curr_msg.price;
                                        if (level_match) begin
                                            top_bid_shares <= level_data.total_shares + curr_msg.shares; 
                                        end
                                        else begin
                                            top_bid_shares <= curr_msg.shares; 
                                        end
                                    end
                                end

                                else begin
                                    if (curr_msg.price == top_ask_price) begin
                                        top_ask_shares <= level_data.total_shares + curr_msg.shares;
                                    end
                                    else if (curr_msg.price < top_ask_price) begin
                                        top_ask_price <= curr_msg.price;
                                        if (level_match) begin
                                            top_ask_shares <= level_data.total_shares + curr_msg.shares;
                                        end
                                        else begin
                                            top_ask_shares <= curr_msg.shares; 
                                        end
                                    end
                                end
                            end
                            else begin
                                overflow_count <= overflow_count + 32'd1;
                            end
                        end

                        // L2 work identical for E/D/X and U's delete half (X is just a partial E with no trade behind it)
                        MSG_DEL, MSG_EXC, MSG_CXL, MSG_RPL: begin
                            // L3 - always_comb assembled bucket_next
                            book_mem[curr_bucket] <= bucket_next;

                            // L2
                            if (level_match) begin
                                if (curr_is_buy) begin
                                        bid_levels[curr_level] <= level_next;
                                    end
                                    else begin
                                        ask_levels[curr_level] <= level_next;
                                    end
                                if (level_data.order_count==16'd1 && order_left) begin
                                    if (curr_is_buy) begin
                                        bid_level_count <= bid_level_count - 1'b1;
                                    end
                                    else begin
                                        ask_level_count <= ask_level_count - 1'b1;
                                    end

                                    // RESCAN CASE - if the level we're removing was top of book
                                    if (curr_is_buy && curr_price==top_bid_price) begin
                                        rescan_is_buy <= 1'b1;
                                        if (bid_level_count == 1) begin
                                            // that was the last bid level, nothing to look for
                                            top_bid_price <= '0;
                                            top_bid_shares <= '0;
                                        end
                                        else begin
                                            walk_price <= tick_away(curr_price, 1'b1);
                                            walk_slot <= price_slot(tick_away(curr_price, 1'b1));
                                            walk_count <= '0;
                                            walk_crossed_dollar <= '0;
                                            rescan_best_price <= '0;
                                            rescan_best_shares <= '0;
                                            state <= WALK;
                                        end
                                    end
                                    else if (!curr_is_buy && curr_price==top_ask_price) begin
                                        rescan_is_buy <= '0;
                                        if (ask_level_count == 1) begin
                                            top_ask_price <= 32'hFFFFFFFF;
                                            top_ask_shares <= '0;
                                        end
                                        else begin
                                            walk_price <= tick_away(curr_price, 1'b0);
                                            walk_slot <= price_slot(tick_away(curr_price, 1'b0));
                                            walk_count <= '0;
                                            walk_crossed_dollar <= '0;
                                            rescan_best_price <= 32'hFFFFFFFF;
                                            rescan_best_shares <= '0;
                                            state <= WALK;
                                        end
                                    end
                                end
                                else begin
                                    // level not removed but just reduced by some shares
                                    if (curr_is_buy && curr_price==top_bid_price) begin
                                        top_bid_shares <= level_data.total_shares - shares_removed;
                                    end
                                    else if (!curr_is_buy && curr_price==top_ask_price) begin
                                        top_ask_shares <= level_data.total_shares - shares_removed;
                                    end
                                end

                                book_valid <= 1'b1;
                            end
                            else begin
                                // order exists in L3 but the price doesn't have a level
                                level_collision_count <= level_collision_count + 32'd1;
                            end
                        end

                        default: ;
                    endcase

                    // U's add half just finished, so the book is whole again: publish it now
                    // (pulse even if the add overflowed, since the delete half already changed the book)
                    if (rpl_pending) begin
                        rpl_pending <= '0;
                        book_valid <= 1'b1;
                    end

                    // U's delete half just ran above, so turn curr_msg into the add half
                    if (curr_msg.msg_type == MSG_RPL) begin
                        curr_msg.msg_type <= MSG_ADD;
                        curr_msg.order_ref_num <= curr_msg.new_order_ref_num;
                        curr_msg.is_buy <= curr_is_buy; // U carries no side, the new order takes the old order's side
                        curr_bucket <= hash_order_ref(curr_msg.new_order_ref_num);
                        rpl_pending <= 1'b1;

                        // don't publish the half-done book (old order out, new one not in yet)
                        // trade_signal acts on book_valid, so it could fire on a top of book that never existed on the exchange
                        book_valid <= '0;
                    end
                end

                WALK: begin
                    rescan_best_price <= rescan_next_price;
                    rescan_best_shares <= rescan_next_shares;

                    if (walk_found) begin
                        if (rescan_is_buy) begin
                            top_bid_price <= walk_level.price;
                            top_bid_shares <= walk_level.total_shares;
                        end
                        else begin
                            top_ask_price <= walk_level.price;
                            top_ask_shares <= walk_level.total_shares;
                        end

                        // a U still has its add half to do (and it publishes once that lands)
                        state <= rpl_pending ? READ_BUCKET : IDLE;
                        book_valid <= !rpl_pending;
                    end
                    else if (walk_count == LEVEL_ADDR_WIDTH'(NUM_LEVELS-1)) begin
                        if (!walk_crossed_dollar) begin
                            // every slot got looked at once, so the best one seen is the new top
                            if (rescan_is_buy) begin
                                top_bid_price <= rescan_next_price;
                                top_bid_shares <= rescan_next_shares;
                            end
                            else begin
                                top_ask_price <= rescan_next_price;
                                top_ask_shares <= rescan_next_shares;
                            end
                            state <= rpl_pending ? READ_BUCKET : IDLE;
                            book_valid <= !rpl_pending;
                        end
                        else begin
                            // some slots got looked at twice and some never, so scan them all
                            rescan_index <= '0;
                            rescan_best_shares <= '0;
                            rescan_best_price <= rescan_is_buy ? 32'd0 : 32'hFFFFFFFF;
                            state <= RESCAN;
                        end
                    end
                    else begin
                        walk_price <= tick_away(walk_price, rescan_is_buy);
                        walk_slot <= price_slot(tick_away(walk_price, rescan_is_buy));
                        walk_count <= walk_count + 1'b1;
                        if ((tick_away(walk_price, rescan_is_buy) >= ONE_DOLLAR) != (walk_price >= ONE_DOLLAR)) begin
                            walk_crossed_dollar <= 1'b1;
                        end
                    end
                end

                RESCAN: begin
                    rescan_best_price <= rescan_next_price;
                    rescan_best_shares <= rescan_next_shares;

                    if (rescan_index == LEVEL_ADDR_WIDTH'(NUM_LEVELS-1)) begin // cast to right size
                        // rescan_next_*, not rescan_best_*, or the last slot never counts
                        if (rescan_is_buy) begin
                            top_bid_shares <= rescan_next_shares;
                            top_bid_price <= rescan_next_price;
                        end
                        else begin
                            top_ask_shares <= rescan_next_shares;
                            top_ask_price <= rescan_next_price;
                        end

                        // a U still has its add half to do (and it publishes once that lands)
                        state <= rpl_pending ? READ_BUCKET : IDLE;
                        book_valid <= !rpl_pending;
                    end

                    else begin // increment scan
                        rescan_index <= rescan_index + 1'b1;
                    end
                end

                default: begin
                    state <= IDLE;
                end
            endcase
        end
    end

    `ifdef SIM
        assert property(
            @(posedge clk) disable iff (!rst_n)
            (best_bid_price != '0 && best_ask_price != 32'hFFFFFFFF) |-> (best_ask_price > best_bid_price)
        ) else $error("book published a crossed market (bid is at or above ask - not possible)");
        // mid-U the top can move with no pulse yet (it gets published when the add half lands)
        assert property(
            @(posedge clk) disable iff (!rst_n)
            !$stable(best_ask_price) |-> (book_valid || rpl_pending)
        ) else $error("ask changed w/o book update");
        assert property(
            @(posedge clk) disable iff (!rst_n)
            !$stable(best_bid_price) |-> (book_valid || rpl_pending)
        ) else $error("bid changed w/o book update");
        assert property(
            @(posedge clk) disable iff (!rst_n)
            rpl_pending |-> (state != IDLE)
        ) else $error("another msg could get in between a U's delete and add halves");
        assert property(
            @(posedge clk) disable iff (!rst_n)
            book_valid |-> !rpl_pending
        ) else $error("published a half-done U");
        assert property(
            @(posedge clk) disable iff (!rst_n)
            $rose(symbol_trading) |-> $past(s_axis_tvalid && s_axis_tready && (s_axis_tdata.stock_locate == cfg_stock_locate)
                                            && (s_axis_tdata.msg_type == MSG_ACT || s_axis_tdata.msg_type == MSG_OPH))
        ) else $error("trading got unblocked by something other than an H/h for our symbol");
        assert property(
            @(posedge clk) disable iff (!rst_n)
            (bid_level_count <= (LEVEL_ADDR_WIDTH+1)'(NUM_LEVELS)) && (ask_level_count <= (LEVEL_ADDR_WIDTH+1)'(NUM_LEVELS))
        ) else $error("level count went past the ladder size (it wrapped below 0)");
        assert property(
            @(posedge clk) disable iff (!rst_n)
            miss_count >= $past(miss_count)
        ) else $error("miss_count decremented somehow");
    `endif

endmodule
