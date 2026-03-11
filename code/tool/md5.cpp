#include "md5.h"
#include <sstream>
#include <iomanip>
#include <fstream>

static const uint8_t PADDING[64] = { 0x80 };
static const uint32_t AC[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };

// MD5 变换常量
static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

static const uint32_t S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

inline uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
inline uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
inline uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
inline uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }
inline uint32_t rotate_left(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

MD5::MD5() {
    count_[0] = count_[1] = 0;
    memcpy(state_, AC, sizeof(state_));
    finalized_ = false;
}

void MD5::update(const uint8_t* input, size_t length) {
    size_t index = count_[0] / 8 % 64;
    count_[0] += length << 3;
    if (count_[0] < (length << 3)) count_[1]++;
    count_[1] += length >> 29;
    
    size_t firstpart = 64 - index;
    size_t i;
    
    if (length >= firstpart) {
        memcpy(&buffer_[index], input, firstpart);
        transform(buffer_);
        for (i = firstpart; i + 64 <= length; i += 64)
            transform(&input[i]);
        index = 0;
    } else {
        i = 0;
    }
    memcpy(&buffer_[index], &input[i], length - i);
}

void MD5::finalize() {
    if (finalized_) return;
    uint8_t bits[8];
    encode(bits, count_, 8);
    size_t index = count_[0] / 8 % 64;
    size_t padLen = (index < 56) ? (56 - index) : (120 - index);
    update(PADDING, padLen);
    update(bits, 8);
    encode(digest_, state_, 16);
    memset(buffer_, 0, sizeof(buffer_));
    finalized_ = true;
}

std::string MD5::hexdigest() const {
    std::ostringstream oss;
    for (int i = 0; i < 16; ++i)
        oss << std::hex << std::setfill('0') << std::setw(2) << (int)digest_[i];
    return oss.str();
}

std::string MD5::hashFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) return "";
    
    MD5 md5;
    char buffer[8192];
    while (file.read(buffer, sizeof(buffer))) {
        md5.update(reinterpret_cast<uint8_t*>(buffer), file.gcount());
    }
    if (file.gcount() > 0) {
        md5.update(reinterpret_cast<uint8_t*>(buffer), file.gcount());
    }
    md5.finalize();
    return md5.hexdigest();
}

std::string MD5::hashData(const void* data, size_t len) {
    MD5 md5;
    md5.update(reinterpret_cast<const uint8_t*>(data), len);
    md5.finalize();
    return md5.hexdigest();
}

void MD5::transform(const uint8_t block[64]) {
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t x[16];
    
    decode(x, block, 64);
    
    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16) {
            f = F(b, c, d);
            g = i;
        } else if (i < 32) {
            f = G(b, c, d);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = H(b, c, d);
            g = (3 * i + 5) % 16;
        } else {
            f = I(b, c, d);
            g = (7 * i) % 16;
        }
        
        uint32_t temp = d;
        d = c;
        c = b;
        b = b + rotate_left(a + f + K[i] + x[g], S[i]);
        a = temp;
    }
    
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void MD5::decode(uint32_t* output, const uint8_t* input, size_t len) {
    for (size_t i = 0, j = 0; j < len; i++, j += 4)
        output[i] = ((uint32_t)input[j]) | (((uint32_t)input[j+1]) << 8) |
                    (((uint32_t)input[j+2]) << 16) | (((uint32_t)input[j+3]) << 24);
}

void MD5::encode(uint8_t* output, const uint32_t* input, size_t len) {
    for (size_t i = 0, j = 0; j < len; i++, j += 4) {
        output[j] = input[i] & 0xff;
        output[j+1] = (input[i] >> 8) & 0xff;
        output[j+2] = (input[i] >> 16) & 0xff;
        output[j+3] = (input[i] >> 24) & 0xff;
    }
}