// Regression tests for the dependency-free PNG writer in png_writer.hpp.
//
// Coverage:
//  * every RFC 1951 length boundary 3..258 and distance boundary 1..32768 maps to
//    the exact code the standard assigns. Code 284 stops at 257 and code 285 is
//    the sole encoding of 258, so a "fits in this code" lookup is caught here;
//  * a strict fixed-Huffman inflater that rejects code 284 + extra 31, plus a
//    hand-built stream that proves the reject path fires;
//  * full PNG/zlib round trips (chunk CRCs, Adler-32, zero filter bytes, pixel
//    equality) over flat, gradient, 1x1, wide and incompressible images.
#include "png_writer.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// RFC 1951 section 3.2.5 ground truth. kLengthMax[27] is 257, not 258: code 284
// is truncated so that 258 has exactly one encoding, code 285.
constexpr int kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int kLengthMax[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 18, 22, 26, 30, 34, 42, 50, 58, 66,
    82, 98, 114, 130, 162, 194, 226, 257, 258};
constexpr int kDistanceBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr int kDistanceExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr int kDistanceMax[30] = {
    1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024,
    1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768};

int expectedLengthIndex(int length) {
    for (int i = 0; i < 29; ++i)
        if (length >= kLengthBase[i] && length <= kLengthMax[i]) return i;
    return -1;
}

int expectedDistanceIndex(int distance) {
    for (int i = 0; i < 30; ++i)
        if (distance >= kDistanceBase[i] && distance <= kDistanceMax[i]) return i;
    return -1;
}

void checkCodeTables() {
    // The ground-truth tables must themselves be contiguous and end correctly.
    for (int i = 0; i + 1 < 29; ++i)
        if (kLengthMax[i] + 1 != kLengthBase[i + 1])
            throw std::runtime_error("length ground-truth table is not contiguous at " + std::to_string(i));
    if (kLengthMax[28] != 258 || kLengthBase[28] != 258)
        throw std::runtime_error("length ground-truth table must end at 258");
    if (kLengthMax[27] != 257)
        throw std::runtime_error("code 284 ground truth must stop at 257");
    for (int i = 0; i + 1 < 30; ++i)
        if (kDistanceMax[i] + 1 != kDistanceBase[i + 1])
            throw std::runtime_error("distance ground-truth table is not contiguous at " + std::to_string(i));
    if (kDistanceMax[29] != 32768)
        throw std::runtime_error("distance ground truth must end at 32768");
}

void checkLengthCodes() {
    for (int length = 3; length <= 258; ++length) {
        const int expected = expectedLengthIndex(length);
        if (expected < 0) throw std::runtime_error("no expected length code for " + std::to_string(length));
        const image::png::LengthEntry& entry = image::png::lengthEntry(length);
        if (entry.symbol != 257 + expected || entry.base != kLengthBase[expected] || entry.extra != kLengthExtra[expected])
            throw std::runtime_error("length " + std::to_string(length) + " mapped to code "
                + std::to_string(entry.symbol) + " (expected " + std::to_string(257 + expected) + ")");
        const int offset = length - entry.base;
        if (offset < 0 || offset >= (1 << entry.extra))
            throw std::runtime_error("length " + std::to_string(length) + " extra bits out of range");
    }
    const image::png::LengthEntry& boundary = image::png::lengthEntry(258);
    if (boundary.symbol != 285 || boundary.base != 258 || boundary.extra != 0)
        throw std::runtime_error("length 258 must be code 285 with no extra bits");
    if (image::png::lengthEntry(257).symbol != 284)
        throw std::runtime_error("length 257 must stay on code 284");
    std::cout << "  length codes 3..258 OK (258 -> 285, 257 -> 284)\n";
}

void checkDistanceCodes() {
    for (int distance = 1; distance <= 32768; ++distance) {
        const int expected = expectedDistanceIndex(distance);
        if (expected < 0) throw std::runtime_error("no expected distance code for " + std::to_string(distance));
        const image::png::DistanceEntry& entry = image::png::distanceEntry(distance);
        if (entry.symbol != expected || entry.base != kDistanceBase[expected] || entry.extra != kDistanceExtra[expected])
            throw std::runtime_error("distance " + std::to_string(distance) + " mapped to code "
                + std::to_string(entry.symbol) + " (expected " + std::to_string(expected) + ")");
        const int offset = distance - entry.base;
        if (offset < 0 || offset >= (1 << entry.extra))
            throw std::runtime_error("distance " + std::to_string(distance) + " extra bits out of range");
    }
    std::cout << "  distance codes 1..32768 OK\n";
}

std::vector<unsigned char> readFile(const std::string& path) {
    std::FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "rb") != 0 || !file) throw std::runtime_error("cannot open " + path);
    std::vector<unsigned char> data;
    unsigned char chunk[4096];
    std::size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) data.insert(data.end(), chunk, chunk + read);
    std::fclose(file);
    return data;
}

uint32_t readBigEndian(const unsigned char* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16)
        | (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

// Minimal inflate for the single final fixed-Huffman block that png_writer emits.
class BitReader {
public:
    BitReader(const unsigned char* data, std::size_t size) : data_(data), size_(size) {}
    unsigned bits(int count) {
        unsigned value = 0;
        for (int i = 0; i < count; ++i) {
            if (position_ >= size_ * 8) throw std::runtime_error("deflate stream ended early");
            const unsigned bit = (data_[position_ / 8] >> (position_ % 8)) & 1u;
            value |= bit << i;
            ++position_;
        }
        return value;
    }
    unsigned code(int count) {
        unsigned value = 0;
        for (int i = 0; i < count; ++i) value = (value << 1) | bits(1);
        return value;
    }
    // zlib pads the final block to a byte boundary with zero bits.
    void requireZeroPadding() const {
        const std::size_t total = size_ * 8;
        if (position_ > total) throw std::runtime_error("read past the deflate stream");
        if (total - position_ >= 8) throw std::runtime_error("trailing bits after the final block");
        for (std::size_t i = position_; i < total; ++i)
            if (((data_[i / 8] >> (i % 8)) & 1u) != 0) throw std::runtime_error("non-zero deflate padding");
    }

private:
    const unsigned char* data_;
    std::size_t size_;
    std::size_t position_ = 0;
};

int fixedSymbol(BitReader& reader) {
    const unsigned seven = reader.code(7);
    if (seven <= 0x17) return 256 + static_cast<int>(seven);
    const unsigned eight = (seven << 1) | reader.bits(1);
    if (eight <= 0xBF) return static_cast<int>(eight - 0x30);
    if (eight <= 0xC7) return 280 + static_cast<int>(eight - 0xC0);
    const unsigned nine = (eight << 1) | reader.bits(1);
    return 144 + static_cast<int>(nine - 0x190);
}

std::vector<unsigned char> inflateFixedStrict(const unsigned char* data, std::size_t size, std::size_t expected) {
    if (size < 6) throw std::runtime_error("zlib stream is too short");
    const unsigned header = (static_cast<unsigned>(data[0]) << 8) | data[1];
    if ((data[0] & 0x0Fu) != 8u) throw std::runtime_error("zlib compression method is not deflate");
    if ((data[1] & 0x20u) != 0u) throw std::runtime_error("zlib FDICT must be clear");
    if ((header % 31u) != 0u) throw std::runtime_error("bad zlib header check bits");
    BitReader reader(data + 2, size - 6);
    if (reader.bits(1) != 1) throw std::runtime_error("expected a final block");
    if (reader.bits(2) != 1) throw std::runtime_error("expected fixed Huffman");
    std::vector<unsigned char> out;
    for (;;) {
        const int symbol = fixedSymbol(reader);
        if (symbol < 256) { out.push_back(static_cast<unsigned char>(symbol)); continue; }
        if (symbol == 256) break;
        const int index = symbol - 257;
        if (index < 0 || index > 28) throw std::runtime_error("bad length symbol");
        const unsigned extra = reader.bits(kLengthExtra[index]);
        // Code 284 is documented as 227-257; using it for 258 is out of range.
        if (index == 27 && extra == 31u)
            throw std::runtime_error("length 258 encoded as code 284 + extra 31 instead of code 285");
        const int copyLength = kLengthBase[index] + static_cast<int>(extra);
        const unsigned distanceSymbol = reader.code(5);
        if (distanceSymbol > 29) throw std::runtime_error("bad distance symbol");
        const int distance = kDistanceBase[distanceSymbol] + static_cast<int>(reader.bits(kDistanceExtra[distanceSymbol]));
        if (distance <= 0 || static_cast<std::size_t>(distance) > out.size())
            throw std::runtime_error("distance past the start of the output");
        for (int i = 0; i < copyLength; ++i)
            out.push_back(out[out.size() - static_cast<std::size_t>(distance)]);
    }
    reader.requireZeroPadding();
    if (out.size() != expected) throw std::runtime_error("inflated size mismatch");
    const uint32_t storedAdler = readBigEndian(data + size - 4);
    if (storedAdler != image::png::adler32(out.data(), out.size())) throw std::runtime_error("bad Adler-32");
    return out;
}

void checkDeflateRoundTrip(const std::vector<unsigned char>& raw, const std::string& label) {
    std::vector<unsigned char> stream;
    image::png::deflate(raw, stream);
    const std::vector<unsigned char> decoded = inflateFixedStrict(stream.data(), stream.size(), raw.size());
    if (decoded != raw) throw std::runtime_error(label + ": deflate round-trip mismatch");
    std::cout << "  deflate " << label << ": " << raw.size() << " -> " << stream.size() << " bytes\n";
}

// A hand-built stream that encodes length 258 through code 284 + extra 31. The
// strict inflater must reject it, proving the regression check is live.
void checkStrictDecoderRejectsTruncated284() {
    std::vector<unsigned char> stream{0x78, 0x01};
    image::png::BitWriter writer(stream);
    writer.bits(1, 1);  // BFINAL
    writer.bits(1, 2);  // BTYPE = fixed Huffman
    image::png::huffmanSymbol(writer, static_cast<unsigned>('A'));
    image::png::huffmanSymbol(writer, 284u);
    writer.bits(31, 5); // extra bits that push code 284 to 258
    writer.code(0, 5);  // distance code 0, distance 1
    image::png::huffmanSymbol(writer, 256u);
    writer.align();
    const std::vector<unsigned char> decoded(259, static_cast<unsigned char>('A'));
    const uint32_t sum = image::png::adler32(decoded.data(), decoded.size());
    stream.push_back(static_cast<unsigned char>(sum >> 24));
    stream.push_back(static_cast<unsigned char>(sum >> 16));
    stream.push_back(static_cast<unsigned char>(sum >> 8));
    stream.push_back(static_cast<unsigned char>(sum));
    bool rejected = false;
    try {
        inflateFixedStrict(stream.data(), stream.size(), decoded.size());
    } catch (const std::exception&) {
        rejected = true;
    }
    if (!rejected) throw std::runtime_error("strict inflater accepted code 284 + extra 31 (length 258)");
    std::cout << "  strict inflater rejects code 284 + extra 31\n";
}

void checkPngRoundTrip(unsigned width, unsigned height, const std::vector<unsigned char>& rgba,
    const std::string& label, bool expectCompression) {
    if (rgba.size() != static_cast<std::size_t>(width) * height * 4)
        throw std::runtime_error(label + ": test buffer size mismatch");
    const std::string path = "png_writer_test_" + label + ".png";
    image::png::write(path, width, height, rgba.data());
    const std::vector<unsigned char> file = readFile(path);
    std::remove(path.c_str());
    const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (file.size() < 57 || std::memcmp(file.data(), signature, 8) != 0)
        throw std::runtime_error(label + ": bad PNG signature");
    std::size_t position = 8;
    bool sawHeader = false, sawData = false, sawEnd = false;
    std::vector<unsigned char> idat;
    while (position + 12 <= file.size()) {
        const unsigned char* chunk = file.data() + position;
        const uint32_t length = readBigEndian(chunk);
        const std::string type(reinterpret_cast<const char*>(chunk + 4), 4);
        if (position + 12 + length > file.size()) throw std::runtime_error(label + ": truncated chunk " + type);
        if (image::png::crc32(chunk + 4, static_cast<std::size_t>(length) + 4) != readBigEndian(chunk + 8 + length))
            throw std::runtime_error(label + ": bad chunk CRC in " + type);
        if (type == "IHDR") {
            if (sawHeader) throw std::runtime_error(label + ": duplicate IHDR");
            sawHeader = true;
            if (readBigEndian(chunk + 8) != width || readBigEndian(chunk + 12) != height
                || chunk[16] != 8 || chunk[17] != 6 || chunk[18] != 0 || chunk[19] != 0 || chunk[20] != 0)
                throw std::runtime_error(label + ": unexpected IHDR fields");
        } else if (type == "IDAT") {
            sawData = true;
            idat.insert(idat.end(), chunk + 8, chunk + 8 + length);
        } else if (type == "IEND") {
            if (length != 0) throw std::runtime_error(label + ": IEND must be empty");
            sawEnd = true;
            break;
        }
        position += 12 + static_cast<std::size_t>(length);
    }
    if (!sawHeader || !sawData || !sawEnd) throw std::runtime_error(label + ": missing required chunks");
    const std::size_t rawSize = static_cast<std::size_t>(height) * (static_cast<std::size_t>(width) * 4 + 1);
    const std::vector<unsigned char> raw = inflateFixedStrict(idat.data(), idat.size(), rawSize);
    for (unsigned y = 0; y < height; ++y) {
        const unsigned char* row = raw.data() + static_cast<std::size_t>(y) * (static_cast<std::size_t>(width) * 4 + 1);
        if (row[0] != 0) throw std::runtime_error(label + ": unexpected filter byte");
        if (std::memcmp(row + 1, rgba.data() + static_cast<std::size_t>(y) * width * 4,
            static_cast<std::size_t>(width) * 4) != 0)
            throw std::runtime_error(label + ": pixel round-trip mismatch");
    }
    if (expectCompression && file.size() >= rawSize)
        throw std::runtime_error(label + ": flat image did not compress");
    std::cout << "  PNG " << label << ": " << width << 'x' << height << ", " << file.size()
              << " bytes for " << rawSize << " raw\n";
}

} // namespace

int main() try {
    checkCodeTables();
    checkLengthCodes();
    checkDistanceCodes();
    checkStrictDecoderRejectsTruncated284();

    checkDeflateRoundTrip(std::vector<unsigned char>(1, 0x2A), "single");
    checkDeflateRoundTrip(std::vector<unsigned char>(2, 0x2A), "two");
    checkDeflateRoundTrip(std::vector<unsigned char>(3, 0x2A), "three");
    checkDeflateRoundTrip(std::vector<unsigned char>(258, 0x2A), "run257");
    checkDeflateRoundTrip(std::vector<unsigned char>(259, 0x2A), "run258");
    checkDeflateRoundTrip(std::vector<unsigned char>(60000, 0x2A), "run60000");
    {
        std::vector<unsigned char> periodic(5000);
        for (std::size_t i = 0; i < periodic.size(); ++i) periodic[i] = static_cast<unsigned char>(i % 3);
        checkDeflateRoundTrip(periodic, "period3");
    }
    {
        std::vector<unsigned char> all(1024);
        for (std::size_t i = 0; i < all.size(); ++i) all[i] = static_cast<unsigned char>(i & 0xFFu);
        checkDeflateRoundTrip(all, "allbytes");
    }

    {
        const unsigned width = 96, height = 64;
        std::vector<unsigned char> flat(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 0; i < flat.size(); i += 4) {
            flat[i] = 18; flat[i + 1] = 26; flat[i + 2] = 45; flat[i + 3] = 255;
        }
        checkPngRoundTrip(width, height, flat, "flat", true);
    }
    {
        const unsigned width = 64, height = 48;
        std::vector<unsigned char> gradient(static_cast<std::size_t>(width) * height * 4);
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                unsigned char* pixel = gradient.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                if (x < width / 2) { pixel[0] = 20; pixel[1] = 40; pixel[2] = 60; }
                else { pixel[0] = static_cast<unsigned char>(x * 3); pixel[1] = static_cast<unsigned char>(y * 5); pixel[2] = 200; }
                pixel[3] = 255;
            }
        }
        checkPngRoundTrip(width, height, gradient, "gradient", false);
    }
    {
        const unsigned width = 1, height = 1;
        const std::vector<unsigned char> single{0x11, 0x22, 0x33, 0xFF};
        checkPngRoundTrip(width, height, single, "single", false);
    }
    {
        const unsigned width = 512, height = 3;
        std::vector<unsigned char> wide(static_cast<std::size_t>(width) * height * 4);
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                unsigned char* pixel = wide.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                pixel[0] = static_cast<unsigned char>((x + y) & 0xFFu);
                pixel[1] = static_cast<unsigned char>((x * 7 + y) & 0xFFu);
                pixel[2] = static_cast<unsigned char>((x ^ y) & 0xFFu);
                pixel[3] = 255;
            }
        }
        checkPngRoundTrip(width, height, wide, "wide", false);
    }
    {
        const unsigned width = 64, height = 64;
        std::vector<unsigned char> noise(static_cast<std::size_t>(width) * height * 4);
        uint32_t state = 0x12345678u;
        for (std::size_t i = 0; i < noise.size(); ++i) {
            state = state * 1664525u + 1013904223u;
            noise[i] = static_cast<unsigned char>(state >> 24);
        }
        checkPngRoundTrip(width, height, noise, "noise", false);
    }

    std::cout << "png_writer_test OK\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
