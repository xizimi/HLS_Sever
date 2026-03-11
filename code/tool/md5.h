#ifndef MD5_H
#define MD5_H

#include <string>
#include <cstdint>
#include <cstring>
#include <fstream>

class MD5 {
public:
    MD5();
    void update(const uint8_t* input, size_t length);
    void finalize();
    std::string hexdigest() const;
    
    // 计算文件 MD5
    static std::string hashFile(const std::string& filepath);
    // 计算内存数据 MD5
    static std::string hashData(const void* data, size_t len);
    
private:
    void transform(const uint8_t block[64]);
    static void decode(uint32_t* output, const uint8_t* input, size_t len);
    static void encode(uint8_t* output, const uint32_t* input, size_t len);
    
    uint32_t state_[4];
    uint32_t count_[2];
    uint8_t buffer_[64];
    uint8_t digest_[16];
    bool finalized_;
};

#endif // MD5_H