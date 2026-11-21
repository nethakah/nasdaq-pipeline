#include "itch_file_reader.hpp"
#include <stdexcept>

// 2 = width of the big-endian length prefix on every record
static constexpr int LEN_PREFIX_BYTES = 2;

// 50 = largest ITCH 5.0 message, NOII 'I' (last field at offset 49, length 1)
// any length outside 1..50 means we lost framing
static constexpr uint16_t MAX_MSG_LEN = 50;

// 1 << 20 = 1 MiB internal buffer; zlib's default is 8 KiB, so 128x fewer refills
static constexpr unsigned GZ_BUFFER_BYTES = 1u << 20;

ItchFileReader::ItchFileReader(const std::string &path){
    f = gzopen(path.c_str(), "rb");
    if (f == nullptr){
        throw std::runtime_error("cannot open " + path);
    }

    // must be set before the first read
    // a throwing constructor never runs the destructor, so close by hand here
    if (gzbuffer(f, GZ_BUFFER_BYTES) != 0){
        gzclose(f);
        f = nullptr;
        throw std::runtime_error("gzbuffer failed on " + path);
    }
}

ItchFileReader::~ItchFileReader(){
    if (f != nullptr){
        gzclose(f);
    }
}

std::string ItchFileReader::gz_error_text(){
    int errnum = Z_OK;
    const char *s = gzerror(f, &errnum);
    return std::string(s) + " (zlib " + std::to_string(errnum) + ")";
}

bool ItchFileReader::next(std::vector<uint8_t> &msg){
    // 1. length prefix
    uint8_t hdr[LEN_PREFIX_BYTES];
    int r = gzread(f, hdr, LEN_PREFIX_BYTES);

    if (r == 0){
        // gzread also returns 0 when the .gz itself is cut off (bad download)
        // zlib flags that as Z_BUF_ERROR, a clean end of stream is Z_OK
        int errnum = Z_OK;
        gzerror(f, &errnum);
        if (errnum != Z_OK){
            throw std::runtime_error("gz stream ended early after message " + std::to_string(n) + ": " + gz_error_text());
        }
        return false;
    }
    if (r < 0){
        throw std::runtime_error("zlib error reading prefix after message " + std::to_string(n) + ": " + gz_error_text());
    }
    if (r != LEN_PREFIX_BYTES){
        throw std::runtime_error("file ends mid length prefix after message " + std::to_string(n));
    }

    // 2. big-endian: high byte first, shifted up by 8 (bits in a byte)
    uint16_t len = (uint16_t)((uint16_t(hdr[0]) << 8) | hdr[1]);

    // 3. sanity
    if (len == 0 || len > MAX_MSG_LEN){
        throw std::runtime_error("bad length " + std::to_string(len) + " at message " + std::to_string(n + 1) + " (lost framing)");
    }

    // 4. body (resize won't allocate once the vector has grown to 50)
    msg.resize(len);
    r = gzread(f, msg.data(), len);
    if (r < 0){
        throw std::runtime_error("zlib error reading message " + std::to_string(n + 1) + ": " + gz_error_text());
    }
    if (r != (int)len){
        throw std::runtime_error("file ends mid message " + std::to_string(n + 1) + " (got " + std::to_string(r) + " of " + std::to_string(len) + " bytes)");
    }

    // 5.
    n++;
    return true;
}
