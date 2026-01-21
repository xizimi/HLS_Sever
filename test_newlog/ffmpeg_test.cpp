#include <iostream>
#include <thread>
#include <string>
#include <cstdlib>
#include <unistd.h>

std::string safePath(const std::string& s) {
    for (char c : s) {
        if (!(c == '/' || c == '.' || c == '_' || c == '-' ||
              (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            throw std::invalid_argument("Unsafe character in path");
        }
    }
    return s;
}

void convertToHLSAsync(std::string input, std::string outputDir) {
    std::thread([input = std::move(input), outputDir = std::move(outputDir)]() {
        try {
            std::string safeIn = safePath(input);
            std::string safeOut = safePath(outputDir);

            if (access(safeIn.c_str(), F_OK) != 0) {
                std::cerr << "[HLS] File not found: " << safeIn << "\n";
                return;
            }

            std::system(("mkdir -p " + safeOut).c_str());

            std::string cmd =
                "ffmpeg -y -i \"" + safeIn + "\" "
                "-c:v libx264 -profile:v baseline -level 3.0 "
                "-c:a aac -ar 44100 "
                "-hls_time 4 -hls_list_size 0 -f hls \""
                + safeOut + "/index.m3u8\" 2>/dev/null";

            std::cout << "[HLS] Running...\n";
            int ret = std::system(cmd.c_str());
            std::cout << "[HLS] " << (ret ? "❌ Failed" : "✅ Success") << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[HLS] Error: " << e.what() << "\n";
        }
    }).detach();
}

// 测试
int main() {
    convertToHLSAsync("../video_data/1.mp4", "../muts_ts/1_out");
    std::this_thread::sleep_for(std::chrono::seconds(10));
}