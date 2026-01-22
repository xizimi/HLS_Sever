#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <cstring>
#include <vector>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cerrno>
#include <sys/stat.h>

class HttpClient {
private:
    std::string server_ip;
    int server_port;
    std::string base_output_dir = "./hls_downloads"; // 默认输出目录

    // 创建多级目录（类似 mkdir -p）
    bool createDirectory(const std::string& dirPath) {
        size_t pos = 0;
        std::string currentPath;
        while ((pos = dirPath.find('/', pos)) != std::string::npos) {
            currentPath = dirPath.substr(0, pos++);
            if (currentPath.empty()) continue;
            if (mkdir(currentPath.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (mkdir(dirPath.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
        return true;
    }

    // 构造安全的保存路径
    std::string getSavePath(const std::string& videoId, const std::string& relativePath) {
        std::string cleanPath = relativePath;
        if (cleanPath.empty() || cleanPath[0] != '/') {
            cleanPath = "/" + cleanPath;
        }
        if (cleanPath.find("..") != std::string::npos) {
            std::cerr << "⚠️ Path traversal detected! Skipping: " << relativePath << "\n";
            return "";
        }

        std::string fullDir = base_output_dir + "/" + videoId;
        if (!createDirectory(fullDir)) {
            std::cerr << "❌ Failed to create directory: " << fullDir << "\n";
            return "";
        }

        std::string savePath = fullDir + cleanPath;
        return savePath;
    }

    // 保存文件到磁盘（二进制模式）
    bool saveFile(const std::string& videoId, const std::string& urlPath, const std::string& content, bool isTs = false) {
        std::string savePath = getSavePath(videoId, urlPath);
        if (savePath.empty()) return false;

        size_t lastSlash = savePath.find_last_of('/');
        if (lastSlash != std::string::npos) {
            std::string dirPart = savePath.substr(0, lastSlash);
            if (!createDirectory(dirPart)) {
                std::cerr << "❌ Failed to create subdirectory: " << dirPart << "\n";
                return false;
            }
        }

        std::ofstream file(savePath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "❌ Failed to open file for writing: " << savePath << "\n";
            return false;
        }
        file.write(content.data(), content.size());
        file.close();

        std::cout << "💾 Saved to: " << savePath << "\n";

        if (isTs && !content.empty()) {
            if ((unsigned char)content[0] == 0x47) {
                std::cout << "✅ Valid TS signature (starts with 0x47)\n";
            } else {
                std::cout << "⚠️ Invalid TS: does NOT start with 0x47!\n";
            }
        }
        return true;
    }

    // 👇 新增：从完整路径中提取 video_id 之后的部分
    std::string getRelativePathAfterVideoId(const std::string& fullPath, const std::string& videoId) {
        std::string prefix = "/video/" + videoId + "/";
        size_t pos = fullPath.find(prefix);
        if (pos != std::string::npos) {
            // 返回 "/1080p/index.m3u8" 这样的形式
            return fullPath.substr(pos + prefix.length() - 1);
        }
        // 如果不匹配，保守返回 basename
        size_t lastSlash = fullPath.find_last_of('/');
        if (lastSlash != std::string::npos) {
            return fullPath.substr(lastSlash);
        }
        return "/" + fullPath;
    }

    std::string extractBody(const std::string& response) {
        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            return response.substr(headerEnd + 4);
        }
        return response;
    }

    std::string sendGetAndGetResponse(const std::string& path) {
        std::cout << "\n📤 Sending HTTP GET to path: " << path << std::endl;

        int client_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (client_socket == -1) {
            std::cerr << "❌ Socket creation failed\n";
            return "";
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        server_addr.sin_addr.s_addr = inet_addr(server_ip.c_str());

        if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
            std::cerr << "❌ Connect failed\n";
            close(client_socket);
            return "";
        }

        std::string request =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + server_ip + ":" + std::to_string(server_port) + "\r\n"
            "Connection: close\r\n"
            "\r\n";

        if (send(client_socket, request.c_str(), request.length(), 0) == -1) {
            std::cerr << "❌ Send failed\n";
            close(client_socket);
            return "";
        }

        char buffer[4096];
        std::string response;
        ssize_t bytes_received;
        while ((bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0)) > 0) {
            response.append(buffer, bytes_received);
        }
        close(client_socket);
        return response;
    }

    std::string resolvePath(const std::string& baseUrl, const std::string& relative) {
        if (relative.empty() || relative[0] == '/') {
            return relative;
        }
        size_t lastSlash = baseUrl.find_last_of('/');
        if (lastSlash == std::string::npos) {
            return "/" + relative;
        }
        return baseUrl.substr(0, lastSlash + 1) + relative;
    }

    std::vector<std::string> parseM3U8(const std::string& body, const std::string& currentPath) {
        std::vector<std::string> segments;
        std::istringstream stream(body);
        std::string line;

        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || line[0] == '#') {
                continue;
            }
            if (line.find(".ts") != std::string::npos || line.find(".m3u8") != std::string::npos) {
                std::string fullPath = resolvePath(currentPath, line);
                segments.push_back(fullPath);
            }
        }
        return segments;
    }

public:
    HttpClient(const std::string& ip, int port) : server_ip(ip), server_port(port) {}

    void setOutputDir(const std::string& dir) {
        if (!dir.empty()) {
            base_output_dir = dir;
        }
    }

    bool fetchAndFollowHLS(const std::string& initialPath) {
        // 提取 video_id: 假设格式为 /video/{id}/xxx
        std::string videoId = "unknown_video";
        size_t start = initialPath.find("/video/");
        if (start != std::string::npos) {
            start += 7; // skip "/video/"
            size_t end = initialPath.find("/", start);
            if (end == std::string::npos) {
                // 尝试匹配 /video/vid_123/master.m3u8
                end = initialPath.rfind("/master.m3u8");
            }
            if (end != std::string::npos && end > start) {
                videoId = initialPath.substr(start, end - start);
            }
        }

        std::cout << "=== Starting HLS-aware fetch ===\n";
        std::cout << "Target: http://" << server_ip << ":" << server_port << initialPath << "\n";
        std::cout << "📁 Output base dir: " << base_output_dir << "\n";

        std::string response = sendGetAndGetResponse(initialPath);
        if (response.empty()) {
            std::cerr << "❌ Initial request failed.\n";
            return false;
        }

        std::string body = extractBody(response);
        bool isM3U8 = (initialPath.find(".m3u8") != std::string::npos);

        // 👇 修复：使用相对于 video_id 的路径
        std::string relPath = getRelativePathAfterVideoId(initialPath, videoId);
        saveFile(videoId, relPath, body, false);

        if (!isM3U8) {
            std::cout << "\n📄 Not an M3U8 file.\n";
            return true;
        }

        std::cout << "\n📄 Parsing M3U8 playlist...\n";
        auto segments = parseM3U8(body, initialPath);

        if (segments.empty()) {
            std::cout << "⚠️ No segments found.\n";
            return true;
        }

        std::cout << "\n🔍 Found " << segments.size() << " segment(s) to fetch:\n";
        for (const auto& seg : segments) {
            std::cout << " - " << seg << "\n";
        }

        for (const auto& segPath : segments) {
            std::cout << "\n--- Fetching segment: " << segPath << " ---\n";
            std::string segResp = sendGetAndGetResponse(segPath);
            if (segResp.empty()) {
                std::cout << "❌ Failed to fetch segment.\n";
                continue;
            }

            std::string segBody = extractBody(segResp);
            bool isNestedM3U8 = (segPath.find(".m3u8") != std::string::npos);

            // 👇 修复：使用相对路径
            std::string segRelPath = getRelativePathAfterVideoId(segPath, videoId);

            if (isNestedM3U8) {
                std::cout << "📄 Nested M3U8 detected. Parsing its segments...\n";
                saveFile(videoId, segRelPath, segBody, false);

                auto nestedSegments = parseM3U8(segBody, segPath);
                for (const auto& ts : nestedSegments) {
                    std::cout << "\n--- Fetching TS from nested playlist: " << ts << " ---\n";
                    std::string tsResp = sendGetAndGetResponse(ts);
                    if (!tsResp.empty()) {
                        std::string tsBody = extractBody(tsResp);
                        // 👇 修复：使用相对路径
                        std::string tsRelPath = getRelativePathAfterVideoId(ts, videoId);
                        saveFile(videoId, tsRelPath, tsBody, true);
                        std::cout << "✅ TS segment saved.\n";
                    } else {
                        std::cout << "❌ Failed to fetch TS segment.\n";
                    }
                }
            } else {
                saveFile(videoId, segRelPath, segBody, true);
                std::cout << "✅ TS segment saved.\n";
            }
        }

        return true;
    }
};

int main(int argc, char* argv[]) {
    std::cout << "=== HLS Debug Client (Save to Custom Folder) ===\n";

    std::string server_ip = "192.168.46.10";
    int server_port = 1316;
    std::string video_id = "vid_1769065051_9383";
    std::string output_dir = "./video_data/hls_downloads"; // 按你的设定

    // 如果你想启用命令行参数，取消下面的注释
    /*
    if (argc >= 2) {
        output_dir = argv[1];
        std::cout << "📁 Using custom output directory: " << output_dir << "\n";
    } else {
        std::cout << "ℹ️  No output directory specified. Using default: " << output_dir << "\n";
        std::cout << "💡 Usage: " << argv[0] << " /your/custom/output/folder\n";
    }
    */

    std::string path = "/video/" + video_id + "/master.m3u8";

    HttpClient client(server_ip, server_port);
    client.setOutputDir(output_dir);
    client.fetchAndFollowHLS(path);

    std::cout << "\n🎉 Done! Check folder: " << output_dir << "/" << video_id << "/\n";
    return 0;
}