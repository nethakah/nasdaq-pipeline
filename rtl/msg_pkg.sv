/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off UNUSEDPARAM */

package msg_pkg;

    typedef enum logic[3:0]{
        MSG_NONE = 4'd0, // no msg or unrecognized type
        MSG_ADD = 4'd1, // add order (A, and F which is an A + MPID)
        MSG_EXC = 4'd2, // order execute (E, and C which is an E + execution price)
        MSG_DEL = 4'd3, // order delete (D)
        MSG_CXL = 4'd4, // order cancel (X), partial: the order shrinks but keeps its place in line
        MSG_RPL = 4'd5 // order replace (U): old order out, new ref in on the SAME side (loses its place in line)
    } msgtype_enum;

    typedef struct packed{
    // declaration order is the bit order for ALL message types (wont match wire format from NASDAQ)
    // commented is wire format @byte location, actual parser handed down format is the order of the fields
        // at the top on purpose so every older field keeps its bit position (tb word indices don't move)
        logic[63:0] new_order_ref_num; // @19 new ref the order goes by after a replace ('U')

        logic[26:0] rsvd0;         // reserved for C++ padding
        logic is_buy;               // @19 1='B'=Buy, 0='S'=Sell
        msgtype_enum msg_type;      // @0 add/execute/delete
        
        logic[15:0] rsvd1;         // reserved for C++ padding
        logic[15:0] stock_locate;  // @1 book array index 
        
        logic[15:0] rsvd2;         // reserved for C++ padding
        logic[47:0] timestamp;     // @5 ns since 12am
        
        logic[63:0] order_ref_num; // @11 book key ('U': the ORIGINAL order being replaced)
        logic[31:0] shares;        // @20 shares to add ('A') / executed shares ('E') / cancelled shares ('X') / new total ('U' @27)
        logic[63:0] stock;         // @24 8-char ASCII symbol ('A')
        logic[31:0] price;         // @32 raw fixed-point (4 decimals implied) ('U': new price @31)
        logic[63:0] match_num;     // @23 match number ('E') (day unique to identify this specific execution)
    } msg_t; // 448b with padding (so every field starts on a 32-bit boundary)

    function automatic logic[5:0] msg_length( // bytes
        input logic[7:0] type_byte
    ); 
        case (type_byte)
            // offset of last field + len of last field (ITCH 5.0 spec section 1)
            // need EVERY type here, even the ones we don't decode, or we can't skip them and lose framing
            // 6b is enough since the longest message is 50B ('I') and 6b holds up to 63

            // system / admin (skipped)
            "S": msg_length = 6'd12; // system event
            "R": msg_length = 6'd39; // stock directory
            "H": msg_length = 6'd25; // stock trading action
            "Y": msg_length = 6'd20; // reg SHO restriction
            "L": msg_length = 6'd26; // market participant position
            "V": msg_length = 6'd35; // MWCB decline level
            "W": msg_length = 6'd12; // MWCB status
            "K": msg_length = 6'd28; // IPO quoting period update
            "J": msg_length = 6'd35; // LULD auction collar
            "h": msg_length = 6'd21; // operational halt

            // the 7 book-changing types
            "A": msg_length = 6'd36; // add order
            "F": msg_length = 6'd40; // add order w/ MPID
            "E": msg_length = 6'd31; // order executed
            "C": msg_length = 6'd36; // order executed w/ price
            "X": msg_length = 6'd23; // order cancel (partial)
            "D": msg_length = 6'd19; // order delete
            "U": msg_length = 6'd35; // order replace

            // trades / auction / other (skipped)
            "P": msg_length = 6'd44; // trade (non-cross)
            "Q": msg_length = 6'd40; // cross trade
            "B": msg_length = 6'd19; // broken trade
            "I": msg_length = 6'd50; // NOII
            "N": msg_length = 6'd20; // retail price improvement indicator
            "O": msg_length = 6'd48; // DLCR price discovery

            default: msg_length = 6'd0; // not an ITCH 5.0 type (only happens if framing is already lost)
        endcase
    endfunction

    function automatic msgtype_enum decode_type(
        input logic[7:0] type_byte
    ); 
        case (type_byte)
            "A": decode_type = MSG_ADD;
            "F": decode_type = MSG_ADD; // same bytes as A through @35, the MPID @36 just gets skipped
            "E": decode_type = MSG_EXC;
            "C": decode_type = MSG_EXC; // same shares @19 as E, the execution price @32 doesn't move the book
            "D": decode_type = MSG_DEL;
            "X": decode_type = MSG_CXL;
            "U": decode_type = MSG_RPL;
            default: decode_type = MSG_NONE; // the other 16 types never touch the book
        endcase
    endfunction

    //

    typedef struct packed{
        logic[13:0] rsvd;      // reserved for C++ padding
        logic valid;            // slot occupied
        logic is_buy;
        logic[15:0] stock_locate;
        logic[31:0] price;
        logic[31:0] shares;
        logic[63:0] order_ref_num;
    } order_entry_t; // 160b with padding (so every field starts on 32-bit boundary)
    
    localparam int ENTRIES_PER_BUCKET = 4; // each bucket has this many entries in 1 memory word (parallel compared)
    localparam int BOOK_ADDR_WIDTH = 10; // 1024 buckets
    localparam int NUM_BUCKETS = 1 << BOOK_ADDR_WIDTH;
    localparam int BOOK_CAP = NUM_BUCKETS * ENTRIES_PER_BUCKET; // 4096 live orders

    typedef order_entry_t[ENTRIES_PER_BUCKET-1:0] bucket_t; // 640b per bucket

    function automatic logic[BOOK_ADDR_WIDTH-1:0] hash_order_ref(
        input logic[63:0] order_ref
    );
        logic[15:0] folded;
        // use XOR folding to get 64b key into a 12b bucket number (mix all 64 bits)
        folded = order_ref[15:0]
                 ^ order_ref[31:16]
                 ^ order_ref[47:32]
                 ^ order_ref[63:48];
        // NOTE: DO NOT just take the low 12b of order_ref bc NASDAQ assigns refs sequentially which is a problem for bursts of Adds
        
        hash_order_ref = folded[BOOK_ADDR_WIDTH-1:0];
    endfunction

    //

    typedef struct packed{
        logic[13:0] rsvd; // word alignment
        logic valid; // level is occupied
        logic[15:0] order_count; // num of orders at this level
        logic[31:0] total_shares; // sum of shares across the orders
        logic[31:0] price; // price represented by this level
    } level_t; // 96b with padding

    localparam int LEVEL_ADDR_WIDTH = 10;
    localparam int NUM_LEVELS = 1 << LEVEL_ADDR_WIDTH;

    function automatic logic[LEVEL_ADDR_WIDTH-1:0] hash_price(
        input logic[31:0] price
    );
        logic[15:0] folded;
        folded = price[15:0] ^ price[31:16];
        hash_price = folded[LEVEL_ADDR_WIDTH-1:0];
    endfunction

    // flat widths for mem declarations because Vivado RAM interface needs plain `logic[W-1:0] mem[DEPTH]` look
    localparam int BUCKET_BITS = $bits(bucket_t); // 640b
    localparam int LEVEL_BITS = $bits(level_t); // 96b

endpackage
