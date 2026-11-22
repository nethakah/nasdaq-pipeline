#ifndef ITCH_MESSAGES_HPP
#define ITCH_MESSAGES_HPP

#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include "contracts.hpp"

// 2 byte fields (stock locate, tracking num)
static inline void write_u16_bigendian(uint8_t *dest, uint16_t value)
{
    REQUIRES(dest != nullptr);
    // caller must give >= 2 writable bytes

    dest[0] = (uint8_t)(value>>8);
    dest[1] = (uint8_t)(value);
}

// 4 byte fields (shares, price)
static inline void write_u32_bigendian(uint8_t *dest, uint32_t value)
{
    REQUIRES(dest != nullptr);
    // caller must give >= 4 writable bytes

    dest[0] = (uint8_t)(value>>24);
    dest[1] = (uint8_t)(value>>16);
    dest[2] = (uint8_t)(value>>8);
    dest[3] = (uint8_t)(value);
}

// 6 byte fields (timestamp)
static inline void write_u48_bigendian(uint8_t *dest, uint64_t value)
{   
    REQUIRES(dest != nullptr);
    // caller must give >= 6 writable bytes
    
    // IGNORE 16 BITS (48 bits r held in a 64-bit variable here)
    // (since obv theres no 6byte number we can use)
    dest[0] = (uint8_t)(value>>40);
    dest[1] = (uint8_t)(value>>32);
    dest[2] = (uint8_t)(value>>24);
    dest[3] = (uint8_t)(value>>16);
    dest[4] = (uint8_t)(value>>8);
    dest[5] = (uint8_t)(value);
}

// 8  byte fields (order ref num, match num)
static inline void write_u64_bigendian(uint8_t *dest, uint64_t value)
{
    REQUIRES(dest != nullptr);
    // caller must give >= 8 writable bytes
    
    dest[0] = (uint8_t)(value>>56);
    dest[1] = (uint8_t)(value>>48);
    dest[2] = (uint8_t)(value>>40);
    dest[3] = (uint8_t)(value>>32);
    dest[4] = (uint8_t)(value>>24);
    dest[5] = (uint8_t)(value>>16);
    dest[6] = (uint8_t)(value>>8);
    dest[7] = (uint8_t)(value);
}

// READERS (opposite of the writers above, for pulling fields out of real data)

// big-endian = high byte first
// so each byte read shifts everything so far up by 8 (bits in a byte) and ORs in at the bottom
static inline uint64_t read_bigendian(const uint8_t *src, int num_bytes){
    REQUIRES(src != nullptr);
    REQUIRES(num_bytes >= 1 && num_bytes <= 8); // 8 bytes = 64b, the widest ITCH field
    // caller must give >= num_bytes readable bytes

    uint64_t value = 0;
    for (int i = 0; i < num_bytes; i++){
        value = (value<<8) | src[i];
    }
    return value;
}

// 2 byte fields (stock locate, tracking num)
static inline uint16_t read_u16_bigendian(const uint8_t *src){
    return (uint16_t)read_bigendian(src, 2);
}

// 4 byte fields (shares, price)
static inline uint32_t read_u32_bigendian(const uint8_t *src){
    return (uint32_t)read_bigendian(src, 4);
}

// 6 byte fields (timestamp)
static inline uint64_t read_u48_bigendian(const uint8_t *src){
    return read_bigendian(src, 6);
}

// 8 byte fields (order ref num, match num)
static inline uint64_t read_u64_bigendian(const uint8_t *src){
    return read_bigendian(src, 8);
}

static inline uint64_t ticker_to_u64(const char *symbol)
{
    REQUIRES(symbol != nullptr);
    //
    
    uint8_t padded[8];
    std::memset(padded, ' ', 8);

    size_t len = std::strlen(symbol);
    if (len > 8){
        len = 8;
    }
    std::memcpy(padded, symbol, len);

    uint64_t res = 0;
    for (int i = 0; i < 8; i++){
        res = (res<<8) | padded[i];
    }

    return res;
}

// bytes 0-18 are the same w A,E,D so we can fix one edit easily
static inline void build_common_header(
    uint8_t *dest,
    char type_char,
    uint16_t stock_locate,
    uint16_t tracking_num,
    uint64_t timestamp,
    uint64_t order_ref_num
){
    REQUIRES(dest != nullptr);
    REQUIRES(type_char == 'A' || type_char == 'E' || type_char == 'C' || type_char == 'X' || type_char == 'D' || type_char == 'U');
    REQUIRES(timestamp <= 0xFFFFFFFFFFFFULL);
    //

    dest[0] = (uint8_t)type_char;
    write_u16_bigendian(&dest[1], stock_locate);
    write_u16_bigendian(&dest[3], tracking_num);
    write_u48_bigendian(&dest[5], timestamp);
    write_u64_bigendian(&dest[11], order_ref_num);
}

// ORDER ADD
static constexpr size_t ORDER_ADD_LEN = 36;
struct OrderAdd{
    uint16_t stock_locate; // @1 book array index
    uint16_t tracking_num; // @3 internal for Nasdaq (parser ignores this)
    uint64_t timestamp; // @5
    uint64_t order_ref_num; // @11
    bool is_buy; // @19
    uint32_t shares; // @20
    const char* stock; // @24
    uint32_t price; // @32
};
static inline void build_order_add(uint8_t *dest, const OrderAdd &msg)
{
    REQUIRES(dest != nullptr);
    REQUIRES(msg.stock != nullptr);
    //
    
    // common fields
    build_common_header(dest, 'A',
                        msg.stock_locate, msg.tracking_num,
                        msg.timestamp, msg.order_ref_num);

    // rest of fields:

    dest[19] = msg.is_buy ? 'B' : 'S'; //@19
    write_u32_bigendian(&dest[20], msg.shares); //@20

    std::memset(&dest[24], ' ', 8); //@24
    size_t len = std::strlen(msg.stock);
    if (len > 8) len = 8;
    std::memcpy(&dest[24], msg.stock, len);

    write_u32_bigendian(&dest[32], msg.price); //@32

    //
    ENSURES(dest[0] == 'A');
}

// ORDER ADD W/ MPID (F)
// same bytes as A through @35, then a 4-char MPID (the firm that posted it) @36
static constexpr size_t ORDER_ADD_MPID_LEN = 40;
static inline void build_order_add_mpid(uint8_t *dest, const OrderAdd &msg, const char *mpid)
{
    REQUIRES(dest != nullptr);
    REQUIRES(mpid != nullptr);
    //

    build_order_add(dest, msg); // bytes 0-35
    dest[0] = 'F'; // then swap the type

    std::memset(&dest[36], ' ', 4); //@36
    size_t len = std::strlen(mpid);
    if (len > 4) len = 4;
    std::memcpy(&dest[36], mpid, len);

    //
    ENSURES(dest[0] == 'F');
}

// ORDER EXECUTED
static constexpr size_t ORDER_EXECUTED_LEN = 31;
struct OrderExecuted{
    uint16_t stock_locate; //@1
    uint16_t tracking_num; //@3
    uint64_t timestamp; //@5
    uint64_t order_ref_num; //@11
    uint32_t shares; //@19
    uint64_t match_num; //@23
};
static inline void build_order_executed(uint8_t *dest, const OrderExecuted &msg)
{
    REQUIRES(dest != nullptr);
    //

    // common fields
    build_common_header(dest, 'E', 
                        msg.stock_locate, msg.tracking_num,
                        msg.timestamp, msg.order_ref_num);  
    // rest of fields
    write_u32_bigendian(&dest[19], msg.shares);
    write_u64_bigendian(&dest[23], msg.match_num);

    //
    ENSURES(dest[0] == 'E');
}

// ORDER EXECUTED W/ PRICE (C)
// same bytes as E through @30, then printable @31 and the execution price @32
static constexpr size_t ORDER_EXECUTED_PRICE_LEN = 36;
struct OrderExecutedPrice{
    uint16_t stock_locate; //@1
    uint16_t tracking_num; //@3
    uint64_t timestamp; //@5
    uint64_t order_ref_num; //@11
    uint32_t shares; //@19
    uint64_t match_num; //@23
    bool printable; //@31 'Y' or 'N' (whether it shows up on the tape)
    uint32_t exec_price; //@32
};
static inline void build_order_executed_price(uint8_t *dest, const OrderExecutedPrice &msg)
{
    REQUIRES(dest != nullptr);
    //

    // common fields
    build_common_header(dest, 'C',
                        msg.stock_locate, msg.tracking_num,
                        msg.timestamp, msg.order_ref_num);
    // rest of fields
    write_u32_bigendian(&dest[19], msg.shares);
    write_u64_bigendian(&dest[23], msg.match_num);
    dest[31] = msg.printable ? 'Y' : 'N';
    write_u32_bigendian(&dest[32], msg.exec_price);

    //
    ENSURES(dest[0] == 'C');
}

// ORDER CANCEL (X)
// partial cancel: the order shrinks by cancelled_shares but keeps its place in line
// carries the ref and how many shares were REMOVED, not how many are left
static constexpr size_t ORDER_CANCEL_LEN = 23;
struct OrderCancel{
    uint16_t stock_locate; //@1
    uint16_t tracking_num; //@3
    uint64_t timestamp; //@5
    uint64_t order_ref_num; //@11
    uint32_t cancelled_shares; //@19
};
static inline void build_order_cancel(uint8_t *dest, const OrderCancel &msg)
{
    REQUIRES(dest != nullptr);
    //

    // common fields
    build_common_header(dest, 'X',
                        msg.stock_locate, msg.tracking_num,
                        msg.timestamp, msg.order_ref_num);
    // rest of fields
    write_u32_bigendian(&dest[19], msg.cancelled_shares);

    //
    ENSURES(dest[0] == 'X');
}

// ORDER DELETE
static constexpr size_t ORDER_DELETE_LEN = 19;
struct OrderDelete{
    uint16_t stock_locate; //@1
    uint16_t tracking_num; //@3
    uint64_t timestamp; //@5
    uint64_t order_ref_num; //@11
};
static inline void build_order_delete(uint8_t *dest, const OrderDelete &msg)
{
    REQUIRES(dest != nullptr);
    //

    // common fields
    build_common_header(dest, 'D',
                        msg.stock_locate, msg.tracking_num,
                        msg.timestamp, msg.order_ref_num);
    // no other fields

    //
    ENSURES(dest[0] == 'D');
}

// ORDER REPLACE (U)
// cancel-replace: the old order is gone and a new ref takes over (new price + shares, loses its place in line)
// no side on the wire since a replace can't change it
static constexpr size_t ORDER_REPLACE_LEN = 35;
struct OrderReplace{
    uint16_t stock_locate; //@1
    uint16_t tracking_num; //@3
    uint64_t timestamp; //@5
    uint64_t orig_order_ref_num; //@11 the order being replaced
    uint64_t new_order_ref_num; //@19 the ref it goes by from now on
    uint32_t shares; //@27 new TOTAL shares (not a change)
    uint32_t price; //@31 new price
};
static inline void build_order_replace(uint8_t *dest, const OrderReplace &msg)
{
    REQUIRES(dest != nullptr);
    //

    // common fields (the original ref sits @11 like every other type's ref)
    build_common_header(dest, 'U',
                        msg.stock_locate, msg.tracking_num,
                        msg.timestamp, msg.orig_order_ref_num);
    // rest of fields
    write_u64_bigendian(&dest[19], msg.new_order_ref_num);
    write_u32_bigendian(&dest[27], msg.shares);
    write_u32_bigendian(&dest[31], msg.price);

    //
    ENSURES(dest[0] == 'U');
}

// TYPES THE BOOK NEVER USES
// every non-book ITCH 5.0 type with its length (spec section 1), for testing that the parser skips them
static constexpr size_t ITCH_MAX_MSG_LEN = 50; // longest message is NOII ('I')

struct SkipType{
    char type;
    size_t len;
};
static constexpr SkipType SKIP_TYPES[] = {
    {'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35}, {'W', 12}, {'K', 28},
    {'J', 35}, {'h', 21}, {'P', 44}, {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48}
};
static constexpr size_t NUM_SKIP_TYPES = sizeof(SKIP_TYPES) / sizeof(SKIP_TYPES[0]);

// body is random bytes ON PURPOSE so some of them look like type codes ('A', 'D', ...)
// the parser has to skip by length, not get fooled into thinking a new message started
static inline void build_skip_msg(uint8_t *dest, const SkipType &t)
{
    REQUIRES(dest != nullptr);
    REQUIRES(t.len > 0 && t.len <= ITCH_MAX_MSG_LEN);
    //

    dest[0] = (uint8_t)t.type;
    for (size_t i = 1; i < t.len; i++){
        dest[i] = (uint8_t)(rand() % 256); // any byte value
    }

    //
    ENSURES(dest[0] == (uint8_t)t.type);
}

// MOLDUDP
static constexpr size_t MOLD_HEADER_LEN = 20;

// write header into dest (caller appends msgs after it)
static inline void build_mold_header(uint8_t *dest, uint64_t sequence, uint16_t msg_count){
    REQUIRES(dest != nullptr);
    //

    std::memset(&dest[0], 'S', 10);
    write_u64_bigendian(&dest[10], sequence);
    write_u16_bigendian(&dest[18], msg_count);

    //
    ENSURES(dest[0] == 'S');
}

// prepend 2-byte len before ITCH msg and return bytes written
static inline size_t build_mold_msg(uint8_t *dest, const uint8_t *itch, uint16_t len){
    REQUIRES(dest != nullptr);
    REQUIRES(itch != nullptr);
    REQUIRES(len > 0);
    //

    write_u16_bigendian(&dest[0], len);
    std::memcpy(&dest[2], itch, len);
    return len + 2;
}

#endif // ITCH_MESSAGES_HPP