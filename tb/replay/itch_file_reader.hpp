#ifndef ITCH_FILE_READER_HPP
#define ITCH_FILE_READER_HPP

#include <zlib.h>
#include <cstdint>
#include <string>
#include <vector>

// streams a NASDAQ TotalView-ITCH 5.0 day file (.gz) one message at a time
// file record = 2-byte big-endian length, then the message (type char at byte 0)
// same as a MoldUDP64 message block, just without the 20-byte packet header
class ItchFileReader{
    public:
        explicit ItchFileReader(const std::string &path);
        ~ItchFileReader();

        // one owner per gz handle; a copy would gzclose the same handle twice
        ItchFileReader(const ItchFileReader &) = delete;
        ItchFileReader &operator=(const ItchFileReader &) = delete;

        // fills msg with one message (type byte first); false at clean EOF
        // caller reuses one vector so there's no heap allocation per message
        bool next(std::vector<uint8_t> &msg);

        // messages read so far
        uint64_t count() const{
            return n;
        }

    private:
        gzFile f = nullptr;
        uint64_t n = 0;

        std::string gz_error_text();
};

#endif // ITCH_FILE_READER_HPP
