/*
clang++ -std=c++17 -O2 -Wall -Wextra -o tb/replay/count_types tb/replay/count_types.cpp tb/replay/itch_file_reader.cpp -lz
./tb/replay/count_types ~/data/itch/12302019.NASDAQ_ITCH50.gz
*/

#include "itch_file_reader.hpp"
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

// ITCH 5.0 length per type byte (spec section 1: offset of last field + its length)
// 0 = not an ITCH 5.0 type
static int expected_len(uint8_t type){
    switch (type){
        // system / admin
        case 'S': return 12; // system event
        case 'R': return 39; // stock directory
        case 'H': return 25; // stock trading action
        case 'Y': return 20; // reg SHO restriction
        case 'L': return 26; // market participant position
        case 'V': return 35; // MWCB decline level
        case 'W': return 12; // MWCB status
        case 'K': return 28; // IPO quoting period update
        case 'J': return 35; // LULD auction collar
        case 'h': return 21; // operational halt

        // the 7 book-changing types
        case 'A': return 36; // add order (matches msg_pkg.sv)
        case 'F': return 40; // add order with MPID attribution
        case 'E': return 31; // order executed (matches msg_pkg.sv)
        case 'C': return 36; // order executed with price
        case 'X': return 23; // order cancel (partial)
        case 'D': return 19; // order delete (matches msg_pkg.sv)
        case 'U': return 35; // order replace

        // trades / auction / other
        case 'P': return 44; // trade (non-cross)
        case 'Q': return 40; // cross trade
        case 'B': return 19; // broken trade
        case 'I': return 50; // NOII
        case 'N': return 20; // retail price improvement indicator
        case 'O': return 48; // DLCR price discovery

        default: return 0;
    }
}

static bool is_book_changing(uint8_t type){
    switch (type){
        case 'A': case 'F': case 'E': case 'C': case 'X': case 'D': case 'U':
            return true;
        default:
            return false;
    }
}

// system event code sits at offset 11 = 1 type + 2 locate + 2 tracking + 6 timestamp
static constexpr int SYS_EVENT_CODE_OFFSET = 11;

// progress line every 50M messages; a full day is hundreds of millions
static constexpr uint64_t PROGRESS_EVERY = 50000000;

int main(int argc, char **argv){
    if (argc != 2){
        std::fprintf(stderr, "usage: %s <NASDAQ_ITCH50.gz>\n", argv[0]);
        return 2;
    }

    // a byte has 2^8 values, so the type byte is the index directly
    uint64_t counts[256] = {};

    auto t0 = std::chrono::steady_clock::now();

    try{
        ItchFileReader rd(argv[1]);
        std::vector<uint8_t> msg;
        uint8_t last_type = 0;
        uint8_t last_event = 0;

        while (rd.next(msg)){
            uint8_t type = msg[0];
            counts[type]++;

            // check 2: length matches the spec for this type
            int want = expected_len(type);
            if (want == 0){
                char hex[8];
                std::snprintf(hex, sizeof(hex), "0x%02x", type);
                throw std::runtime_error(std::string("unknown type ") + hex + " at message " + std::to_string(rd.count()));
            }
            if ((int)msg.size() != want){
                std::string what = std::string("type ") + (char)type + " is " + std::to_string(msg.size()) + " bytes";
                throw std::runtime_error(what + ", spec says " + std::to_string(want) + " (message " + std::to_string(rd.count()) + ")");
            }

            // check 1a: day starts with S 'O' (start of messages)
            if (rd.count() == 1){
                if (type != 'S' || msg[SYS_EVENT_CODE_OFFSET] != 'O'){
                    throw std::runtime_error("first message is not S 'O'");
                }
            }

            last_type = type;
            last_event = (type == 'S') ? msg[SYS_EVENT_CODE_OFFSET] : 0;

            if (rd.count() % PROGRESS_EVERY == 0){
                std::fprintf(stderr, "  %llu messages...\n", (unsigned long long)rd.count());
            }
        }

        // check 1b: day ends with S 'C' (end of messages)
        if (last_type != 'S' || last_event != 'C'){
            throw std::runtime_error("last message is not S 'C' (truncated file?)");
        }

        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        uint64_t total = rd.count();
        uint64_t book = 0;

        std::printf("type count\n");
        for (int t = 0; t < 256; t++){
            if (counts[t] != 0){
                const char *tag = is_book_changing((uint8_t)t) ? " (book)" : "";
                std::printf("%c %llu%s\n", (char)t, (unsigned long long)counts[t], tag);
                if (is_book_changing((uint8_t)t)){
                    book += counts[t];
                }
            }
        }

        std::printf("\ntotal: %llu\n", (unsigned long long)total);
        double book_pct = 100.0 * (double)book / (double)total;
        std::printf("book-changing: %llu (%.1f%%)\n", (unsigned long long)book, book_pct);
        std::printf("elapsed: %.1f s (%.1f M msgs/s)\n", secs, (double)total / secs / 1e6);
        std::printf("\nPASSED (first S 'O', last S 'C', every length matches spec)\n");
    }
    catch (const std::exception &e){
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }

    return 0;
}
