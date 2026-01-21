#ifndef HEX_H
#define HEX_H
#include <iomanip>
#include <sstream>

inline std::string HexDump(const void* data, size_t len) {
    if (len == 0) return "(empty)";
    
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    std::ostringstream oss;
    oss << "HEX DUMP [" << len << " bytes]:\n";

    // 每行 16 字节
    for (size_t i = 0; i < len; i += 16) {
        // 地址偏移
        oss << std::hex << std::setfill('0') << std::setw(8) << i << ": ";
        
        // 十六进制部分（16 字节）
        for (int j = 0; j < 16; ++j) {
            if (i + j < len) {
                oss << std::hex << std::setfill('0') << std::setw(2) 
                    << static_cast<int>(bytes[i + j]) << " ";
            } else {
                oss << "   "; // 填空格
            }
        }
        
        oss << " | ";
        
        // ASCII 部分（可打印字符）
        for (int j = 0; j < 16 && (i + j) < len; ++j) {
            unsigned char c = bytes[i + j];
            if (c >= 32 && c <= 126) { // 可打印 ASCII
                oss << static_cast<char>(c);
            } else {
                oss << '.';
            }
        }
        oss << "\n";
    }
    return oss.str();
}
#endif