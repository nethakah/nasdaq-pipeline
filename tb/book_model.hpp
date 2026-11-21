#ifndef BOOK_MODEL_HPP
#define BOOK_MODEL_HPP

#include <cstdint>
#include <cstddef>
#include <map>
#include "contracts.hpp"

// reference model of ONE symbol's book (one of these per stock_locate for a full feed)
// built on purpose as a different structure than the RTL (sorted trees, no hashing, no capacity limit)
// so the model and the RTL can't share a bug
//
// the 7 book-changing ITCH 5.0 types and what each one calls:
// A (add) -> add_order
// F (add w/ MPID) -> add_order (MPID is just a firm tag, the book ignores it)
// E (executed) -> exc_order
// C (executed w/ price) -> exc_order (book still shrinks the order at its RESTING price)
// X (partial cancel) -> cancel_order
// D (delete) -> del_order
// U (replace) -> replace_order (old order out, new ref in on the same side)

class BookModel{
    private:
        struct Order{
            bool is_buy;
            uint32_t price;
            uint32_t shares;
        };

        // everything resting at one price
        struct Level{
            uint64_t shares; // 64b so the model can't wrap (RTL keeps 32b, peak_level_shares says if that's enough)
            uint32_t num_orders; // how many orders make up this level
        };

        // <key, value>, variable_name;
        std::map<uint64_t, Order> orders;
        std::map<uint32_t, Level> bid_levels; // best bid = LAST key (highest price)
        std::map<uint32_t, Level> ask_levels; // best ask = FIRST key (lowest price)

        uint32_t miss_count = 0;
        uint64_t peak_level_shares = 0;

        void add_to_level(bool is_buy, uint32_t price, uint32_t shares){
            std::map<uint32_t, Level> &levels = is_buy? bid_levels : ask_levels;

            // [] makes a zeroed level if nothing rests at this price yet
            Level &level = levels[price];
            level.shares += shares;
            level.num_orders++;

            if (level.shares > peak_level_shares){
                peak_level_shares = level.shares;
            }
        }

        // order_gone = these were the order's last shares, so it leaves the level too
        void remove_from_level(bool is_buy, uint32_t price, uint32_t shares, bool order_gone){
            std::map<uint32_t, Level> &levels = is_buy? bid_levels : ask_levels;

            auto i = levels.find(price);
            ASSERT(i != levels.end());
            ASSERT(i->second.shares >= shares);

            i->second.shares -= shares;
            if (order_gone){
                ASSERT(i->second.num_orders > 0);
                i->second.num_orders--;
            }

            // level is empty once its last order leaves
            if (i->second.num_orders == 0){
                ASSERT(i->second.shares == 0);
                levels.erase(i);
            }
        }

        // shared by E, C and X: take shares off an order, remove it once nothing is left
        // (asking for more than what's left just clears the order)
        void reduce_order(std::map<uint64_t, Order>::iterator i, uint32_t shares){
            Order &o = i->second; // value half of key,value pair

            if (shares >= o.shares){
                remove_from_level(o.is_buy, o.price, o.shares, true);
                orders.erase(i);
            }
            else{
                remove_from_level(o.is_buy, o.price, shares, false);
                o.shares -= shares;
            }
        }
    public:
        static constexpr uint32_t BID_EMPTY = 0;
        static constexpr uint32_t ASK_EMPTY = 0xFFFFFFFF;

        bool has_order(uint64_t ref_num){
            return (orders.count(ref_num) > 0);
        }

        // A and F
        void add_order(
            uint64_t ref_num,
            bool is_buy,
            uint32_t price,
            uint32_t shares
        ){
            REQUIRES(shares > 0);
            REQUIRES(orders.count(ref_num) == 0);

            // build the struct
            orders[ref_num] = Order{is_buy, price, shares};
            add_to_level(is_buy, price, shares);
        }

        // E and C
        void exc_order(uint64_t ref_num, uint32_t executed_shares){
            auto i = orders.find(ref_num);
            if (i == orders.end()){
                miss_count++;
                return;
            }

            reduce_order(i, executed_shares);
        }

        // X (partial cancel: the order stays, it just shrinks)
        void cancel_order(uint64_t ref_num, uint32_t cancelled_shares){
            auto i = orders.find(ref_num);
            if (i == orders.end()){
                miss_count++;
                return;
            }

            reduce_order(i, cancelled_shares);
        }

        // D
        void del_order(uint64_t ref_num){
            auto i = orders.find(ref_num);
            if (i == orders.end()){
                miss_count++;
                return;
            }

            Order &o = i->second;
            remove_from_level(o.is_buy, o.price, o.shares, true);
            orders.erase(i);
        }

        // U (cancel-replace)
        // U carries no side, so the new order takes the old order's side
        // new_shares is the new TOTAL, not a change in shares
        void replace_order(
            uint64_t old_ref_num,
            uint64_t new_ref_num,
            uint32_t new_price,
            uint32_t new_shares
        ){
            auto i = orders.find(old_ref_num);
            if (i == orders.end()){
                miss_count++;
                return;
            }

            bool is_buy = i->second.is_buy; // grab it before the old order is gone

            del_order(old_ref_num);
            add_order(new_ref_num, is_buy, new_price, new_shares);
        }

        uint32_t best_bid_price(){
            if (bid_levels.empty()){
                return BID_EMPTY;
            }
            return bid_levels.rbegin()->first; // last to first
        }
        uint32_t best_ask_price(){
            if (ask_levels.empty()){
                return ASK_EMPTY;
            }
            return ask_levels.begin()->first;
        }

        // cut down to 32b to match what the RTL reports
        uint32_t best_bid_shares(){
            if (bid_levels.empty()){
                return 0;
            }
            return (uint32_t)bid_levels.rbegin()->second.shares; // last to first
        }
        uint32_t best_ask_shares(){
            if (ask_levels.empty()){
                return 0;
            }
            return (uint32_t)ask_levels.begin()->second.shares;
        }

        uint32_t spread(){
            if (best_bid_price() == BID_EMPTY){
                return 0xFFFFFFFF;
            }
            if (best_ask_price() == ASK_EMPTY){
                return 0xFFFFFFFF;
            }

            return best_ask_price() - best_bid_price();
        }

        uint32_t get_miss_count(){
            return miss_count;
        }

        // STATS (for sizing the RTL book against real data)

        size_t live_orders(){
            return orders.size();
        }

        // price levels per side (RTL ladder has 1024 slots per side)
        size_t bid_level_count(){
            return bid_levels.size();
        }
        size_t ask_level_count(){
            return ask_levels.size();
        }

        // orders resting in the best num_levels prices of each side
        // = what has to stay on chip if only the top of the book is kept fast
        size_t orders_in_top_levels(size_t num_levels){
            size_t count = 0;

            size_t levels_seen = 0;
            for (auto i = bid_levels.rbegin(); i != bid_levels.rend(); i++){ // best bid first
                if (levels_seen == num_levels){
                    break;
                }
                count += i->second.num_orders;
                levels_seen++;
            }

            levels_seen = 0;
            for (auto i = ask_levels.begin(); i != ask_levels.end(); i++){ // best ask first
                if (levels_seen == num_levels){
                    break;
                }
                count += i->second.num_orders;
                levels_seen++;
            }

            return count;
        }

        // most shares ever resting at one price (RTL level total is 32b, so 4294967295 max)
        uint64_t get_peak_level_shares(){
            return peak_level_shares;
        }
};

#endif // BOOK_MODEL_HPP
