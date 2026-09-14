#pragma once
// Dependency-free 8-bit RGBA PNG writer used by the offscreen frame export.
//
// The project deliberately ships no image or compression library, so the encoder
// emits a single deflate stream with fixed Huffman codes (RFC 1951 §3.2.6) and a
// greedy LZ77 match finder. Diagram frames are mostly flat colour, which that
// matcher compresses well, and the output is a normal PNG any viewer can open.
//
// Ported from the reviewed export branch with one correctness fix: the length
// code lookup tested "fits in this code", which put a 258-byte match on code 284
// with 5 extra bits because 227 + 31 == 258. RFC 1951 reserves code 285 for 258
// and truncates code 284 at 257, so the lookup now selects the greatest base <=
// length. That is decodable by zlib either way, but code 284 + extra 31 is outside
// the code's documented range and strict decoders may reject it.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace image {
namespace png {
inline uint32_t crc32(const unsigned char* data, std::size_t length) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < length; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

inline uint32_t adler32(const unsigned char* data, std::size_t length) {
    uint32_t a = 1, b = 0;
    std::size_t i = 0;
    // 5552 bytes is the largest block whose sums cannot overflow 32 bits.
    while (i < length) {
        const std::size_t block = std::min<std::size_t>(5552, length - i);
        for (std::size_t k = 0; k < block; ++k) { a += data[i + k]; b += a; }
        a %= 65521u; b %= 65521u;
        i += block;
    }
    return (b << 16) | a;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<unsigned char>& out) : out_(out) {}
    // Extra bits and stored bytes are LSB-first; Huffman codes are MSB-first.
    void bits(uint32_t value, int count) {
        for (int i = 0; i < count; ++i) push(((value >> i) & 1u) != 0);
    }
    void code(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) push(((value >> i) & 1u) != 0);
    }
    void align() {
        if (!count_) return;
        out_.push_back(static_cast<unsigned char>(buffer_));
        buffer_ = 0; count_ = 0;
    }

private:
    void push(bool bit) {
        if (bit) buffer_ |= 1u << count_;
        if (++count_ == 8) { out_.push_back(static_cast<unsigned char>(buffer_)); buffer_ = 0; count_ = 0; }
    }
    std::vector<unsigned char>& out_;
    uint32_t buffer_ = 0;
    int count_ = 0;
};

struct LengthEntry { int base, extra, symbol; };
struct DistanceEntry { int base, extra, symbol; };

inline const LengthEntry& lengthEntry(int length) {
    static const LengthEntry table[] = {
        {3,0,257},{4,0,258},{5,0,259},{6,0,260},{7,0,261},{8,0,262},{9,0,263},{10,0,264},
        {11,1,265},{13,1,266},{15,1,267},{17,1,268},
        {19,2,269},{23,2,270},{27,2,271},{31,2,272},
        {35,3,273},{43,3,274},{51,3,275},{59,3,276},
        {67,4,277},{83,4,278},{99,4,279},{115,4,280},
        {131,5,281},{163,5,282},{195,5,283},{227,5,284},
        {258,0,285}};
    // Code ranges are contiguous in `base`: code i covers [base_i, base_{i+1}-1],
    // and the last code spans its whole extra-bit range. Picking the greatest base
    // <= length is the only lookup that keeps 258 on code 285: code 284's base 227
    // plus its 5 extra bits reaches 258, so a "fits in this code" test is wrong.
    const LengthEntry* result = &table[0];
    for (const auto& entry : table)
        if (length >= entry.base) result = &entry;
    return *result;
}

inline const DistanceEntry& distanceEntry(int distance) {
    static const DistanceEntry table[] = {
        {1,0,0},{2,0,1},{3,0,2},{4,0,3},{5,1,4},{7,1,5},{9,2,6},{13,2,7},
        {17,3,8},{25,3,9},{33,4,10},{49,4,11},{65,5,12},{97,5,13},{129,6,14},{193,6,15},
        {257,7,16},{385,7,17},{513,8,18},{769,8,19},{1025,9,20},{1537,9,21},{2049,10,22},{3073,10,23},
        {4097,11,24},{6145,11,25},{8193,12,26},{12289,12,27},{16385,13,28},{24577,13,29}};
    const DistanceEntry* result = &table[0];
    for (const auto& entry : table)
        if (distance >= entry.base) result = &entry;
    return *result;
}

inline void huffmanSymbol(BitWriter& writer, unsigned symbol) {
    if (symbol <= 143) writer.code(0x30u + symbol, 8);
    else if (symbol <= 255) writer.code(0x190u + (symbol - 144), 9);
    else if (symbol <= 279) writer.code(symbol - 256, 7);
    else writer.code(0xC0u + (symbol - 280), 8);
}

// Fixed-Huffman deflate with a greedy 3-byte hash chain. Appends a complete zlib
// stream (header and Adler-32 checksum included) to `out`.
inline void deflate(const std::vector<unsigned char>& raw, std::vector<unsigned char>& out) {
    constexpr int minMatch = 3, maxMatch = 258, window = 32768, hashBits = 15;
    constexpr int hashSize = 1 << hashBits;
    out.push_back(0x78); out.push_back(0x01); // CM=8, 32 KiB window, FLEVEL=0, check bits valid
    BitWriter writer(out);
    writer.bits(1, 1); // final block
    writer.bits(1, 2); // fixed Huffman
    const int size = static_cast<int>(raw.size());
    std::vector<int> head(hashSize, -1), previous(static_cast<std::size_t>(size), -1);
    auto hash3 = [&](int position) {
        const uint32_t value = (static_cast<uint32_t>(raw[position]) << 16)
            | (static_cast<uint32_t>(raw[position + 1]) << 8) | raw[position + 2];
        return static_cast<int>((value * 2654435761u) >> (32 - hashBits));
    };
    auto insert = [&](int position) {
        if (position + minMatch > size) return;
        const int slot = hash3(position);
        previous[static_cast<std::size_t>(position)] = head[slot];
        head[slot] = position;
    };
    int position = 0;
    while (position < size) {
        int bestLength = 0, bestDistance = 0;
        if (position + minMatch <= size) {
            int candidate = head[hash3(position)];
            const int limit = std::min(maxMatch, size - position);
            for (int chain = 0; candidate >= 0 && chain < 24 && position - candidate <= window; ++chain) {
                int length = 0;
                while (length < limit && raw[candidate + length] == raw[position + length]) ++length;
                if (length > bestLength) { bestLength = length; bestDistance = position - candidate; if (length == limit) break; }
                candidate = previous[static_cast<std::size_t>(candidate)];
            }
        }
        if (bestLength >= minMatch) {
            const auto& length = lengthEntry(bestLength);
            huffmanSymbol(writer, static_cast<unsigned>(length.symbol));
            writer.bits(static_cast<uint32_t>(bestLength - length.base), length.extra);
            const auto& distance = distanceEntry(bestDistance);
            writer.code(static_cast<uint32_t>(distance.symbol), 5);
            writer.bits(static_cast<uint32_t>(bestDistance - distance.base), distance.extra);
            const int end = position + bestLength;
            for (int p = position; p < end; ++p) insert(p);
            position = end;
        } else {
            huffmanSymbol(writer, raw[position]);
            insert(position);
            ++position;
        }
    }
    huffmanSymbol(writer, 256); // end of block
    writer.align();
    const uint32_t sum = adler32(raw.data(), raw.size());
    out.push_back(static_cast<unsigned char>(sum >> 24));
    out.push_back(static_cast<unsigned char>(sum >> 16));
    out.push_back(static_cast<unsigned char>(sum >> 8));
    out.push_back(static_cast<unsigned char>(sum));
}

inline void appendChunk(std::vector<unsigned char>& file, const char* type, const std::vector<unsigned char>& data) {
    const uint32_t length = static_cast<uint32_t>(data.size());
    for (int shift = 24; shift >= 0; shift -= 8) file.push_back(static_cast<unsigned char>(length >> shift));
    std::vector<unsigned char> body;
    body.reserve(data.size() + 4);
    for (int i = 0; i < 4; ++i) body.push_back(static_cast<unsigned char>(type[i]));
    body.insert(body.end(), data.begin(), data.end());
    file.insert(file.end(), body.begin(), body.end());
    const uint32_t crc = crc32(body.data(), body.size());
    for (int shift = 24; shift >= 0; shift -= 8) file.push_back(static_cast<unsigned char>(crc >> shift));
}

// `rgba` is a tightly packed top-down RGBA buffer of width*height*4 bytes.
inline void write(const std::filesystem::path& path, unsigned width, unsigned height, const unsigned char* rgba) {
    if (!width || !height || !rgba) throw std::runtime_error("PNG export needs a non-empty RGBA image");
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<std::size_t>(height) * (static_cast<std::size_t>(width) * 4 + 1));
    for (unsigned y = 0; y < height; ++y) {
        raw.push_back(0); // filter type None
        const unsigned char* row = rgba + static_cast<std::size_t>(y) * width * 4;
        raw.insert(raw.end(), row, row + static_cast<std::size_t>(width) * 4);
    }
    std::vector<unsigned char> compressed;
    compressed.reserve(raw.size() / 4);
    deflate(raw, compressed);
    std::vector<unsigned char> header;
    auto push32 = [&](std::vector<unsigned char>& target, uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) target.push_back(static_cast<unsigned char>(value >> shift));
    };
    push32(header, width); push32(header, height);
    header.push_back(8);  // bit depth
    header.push_back(6);  // colour type RGBA
    header.push_back(0);  // deflate
    header.push_back(0);  // adaptive filtering
    header.push_back(0);  // no interlace
    std::vector<unsigned char> file;
    const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    file.insert(file.end(), signature, signature + 8);
    appendChunk(file, "IHDR", header);
    appendChunk(file, "IDAT", compressed);
    appendChunk(file, "IEND", {});
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("Cannot open PNG output: " + path.u8string());
    stream.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    stream.close();
    if (!stream) throw std::runtime_error("Cannot write PNG output: " + path.u8string());
}
} // namespace png
} // namespace image
