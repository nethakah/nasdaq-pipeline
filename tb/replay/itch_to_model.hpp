#ifndef ITCH_TO_MODEL_HPP
#define ITCH_TO_MODEL_HPP

#include "../book_model.hpp"
#include "../itch_messages.hpp"
#include <cstdint>
#include <vector>

// pulls the fields out of 1 real day-file message and applies it to a BookModel
// shared by replay_book and replay_rtl so the offsets only live in 1 place
// offsets are straight from the ITCH 5.0 spec tables

static inline bool is_book_changing(uint8_t type){
    switch (type){
        case 'A': case 'F': case 'E': case 'C': case 'X': case 'D': case 'U':
            return true;
        default:
            return false;
    }
}

static inline void apply_to_book(BookModel &book, const std::vector<uint8_t> &msg){
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

#endif // ITCH_TO_MODEL_HPP
