#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <cstring>
#include <vector>
#include <sstream>
#include <regex>

class HttpClient {
private:
    std::string server_ip;
    int server_port;

    // 辅助函数：发送 GET 并返回完整响应（不含自动递归）
    std::string sendGetAndGetResponse(const std::string& path) {
        int client_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (client_socket == -1) {
            std::cerr << "❌ Socket creation failed for: " << path << std::endl;
            return "";
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        server_addr.sin_addr.s_addr = inet_addr(server_ip.c_str());

        if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
            std::cerr << "❌ Connect failed for: " << path << std::endl;
            close(client_socket);
            return "";
        }

        std::string request =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + server_ip + ":" + std::to_string(server_port) + "\r\n"
            "Connection: close\r\n"
            "\r\n";

        if (send(client_socket, request.c_str(), request.length(), 0) == -1) {
            std::cerr << "❌ Send failed for: " << path << std::endl;
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

    // 解析 m3u8 内容，提取 .ts 文件列表（仅支持简单相对路径）
    std::vector<std::string> parseM3U8(const std::string& m3u8Content, const std::string& basePath) {
        std::vector<std::string> tsFiles;
        std::istringstream stream(m3u8Content);
        std::string line;

        while (std::getline(stream, line)) {
            // 去掉 \r（Windows 换行）
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            // 跳过注释和空行
            if (line.empty() || line[0] == '#') {
                continue;
            }
            // 假设这一行是 .ts 文件名（相对路径）
            if (line.find(".ts") != std::string::npos) {
                // 构造完整 URL 路径
                std::string fullPath = basePath + line;
                // 规范化路径（简单处理，避免 //）
                size_t pos;
                while ((pos = fullPath.find("//")) != std::string::npos) {
                    fullPath.replace(pos, 2, "/");
                }
                tsFiles.push_back(fullPath);
            }
        }
        return tsFiles;
    }

public:
    HttpClient(const std::string& ip, int port) : server_ip(ip), server_port(port) {}

    bool sendGetRequest(const std::string& path) {
        std::cout << "\n🚀 Sending initial GET request to: " << path << std::endl;
        std::string response = sendGetAndGetResponse(path);
        if (response.empty()) {
            std::cerr << "❌ Initial request failed." << std::endl;
            return false;
        }

        // 打印初始响应（比如 API 或 m3u8）
        size_t header_end = response.find("\r\n\r\n");
        std::string headers = (header_end != std::string::npos) ? response.substr(0, header_end) : response;
        std::string body = (header_end != std::string::npos) ? response.substr(header_end + 4) : "";

        std::cout << "\n=== Initial Response Headers ===\n" << headers << "\n";
        std::cout << "================================\n";

        // 判断是否是 m3u8 响应（通过 Content-Type 或路径）
        bool isM3U8 = (path.find(".m3u8") != std::string::npos) ||
                      (headers.find("Content-Type: application/vnd.apple.mpegurl") != std::string::npos) ||
                      (headers.find("Content-Type: application/x-mpegURL") != std::string::npos);

        if (isM3U8) {
            std::cout << "\n📄 Detected HLS playlist (.m3u8). Parsing...\n";
            std::cout << ">>> M3U8 Content <<<\n" << body << "\n";

            // 提取 base path（去掉文件名）
            size_t lastSlash = path.find_last_of('/');
            std::string basePath = (lastSlash != std::string::npos) ? path.substr(0, lastSlash) : "";

            auto tsPaths = parseM3U8(body, basePath);
            if (tsPaths.empty()) {
                std::cout << "⚠️ No .ts segments found in m3u8.\n";
                return true;
            }

            std::cout << "\n🔍 Found " << tsPaths.size() << " .ts segment(s). Fetching...\n";
            for (const auto& tsPath : tsPaths) {
                std::cout << "\n--- Fetching TS: " << tsPath << " ---\n";
                std::string tsResponse = sendGetAndGetResponse(tsPath);
                if (tsResponse.empty()) {
                    std::cout << "❌ Failed to fetch " << tsPath << "\n";
                    continue;
                }

                // 只打印 headers（避免二进制 body 污染终端）
                size_t tsHeaderEnd = tsResponse.find("\r\n\r\n");
                std::string tsHeaders = (tsHeaderEnd != std::string::npos) 
                    ? tsResponse.substr(0, tsHeaderEnd) 
                    : tsResponse;

                std::cout << "TS Response Headers:\n" << tsHeaders << "\n";
                std::cout << "✅ Successfully fetched " << tsPath << "\n";
            }
        } else {
            // 普通响应（如 JSON API），只打印 body
            std::cout << "\n>>> Response Body <<<\n" << body << "\n";
        }

        return true;
    }
};

int main() {
    std::cout << "=== HLS-aware HTTP GET Client ===" << std::endl;

    std::string server_ip = "192.168.46.10";
    int server_port = 1316;

    // 👇 测试两种场景：
    // 场景1: 直接测 m3u8
    // std::string path = "/hls/vid_1768914651_9383/index.m3u8";

    // 场景2: 测 API，它返回 m3u8 路径（但本客户端不会自动 follow redirect，所以建议直接测 m3u8）
    std::string path = "/video?id=vid_1768968685_9383";

    std::cout << "Target: http://" << server_ip << ":" << server_port << path << std::endl;

    HttpClient client(server_ip, server_port);
    client.sendGetRequest(path);

    return 0;
}