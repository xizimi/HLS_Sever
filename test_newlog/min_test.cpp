#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include "../code/tool/md5.h"

class VideoUploader {
private:
    std::string server_ip;
    int server_port;
    
    // 计算文件 MD5
    std::string calculateFileMD5(const std::string& filepath) {
        return MD5::hashFile(filepath);
    }
    
    // 计算分片 MD5
    std::string calculateChunkMD5(const void* data, size_t len) {
        return MD5::hashData(data, len);
    }

public:
    VideoUploader(const std::string& ip, int port) : server_ip(ip), server_port(port) {}

    // 新增：检查文件是否已存在（秒传）
    bool checkFileExists(const std::string& file_md5) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;

        struct sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(server_port);
        if (inet_pton(AF_INET, server_ip.c_str(), &serv_addr.sin_addr) <= 0) {
            close(sock);
            return false;
        }

        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            close(sock);
            return false;
        }

        // 构造检查请求
        std::string json_body = "{\"file_md5\":\"" + file_md5 + "\"}";
        std::ostringstream req;
        req << "POST /upload/check HTTP/1.1\r\n";
        req << "Host: " << server_ip << ":" << server_port << "\r\n";
        req << "Content-Type: application/json\r\n";
        req << "Content-Length: " << json_body.size() << "\r\n";
        req << "Connection: close\r\n\r\n";
        req << json_body;

        std::string request = req.str();
        send(sock, request.c_str(), request.size(), 0);

        char resp_buf[4096];
        ssize_t n = recv(sock, resp_buf, sizeof(resp_buf), 0);
        close(sock);
        
        if (n > 0) {
            std::string resp(resp_buf, n);
            // 检查响应中是否包含 exists:true
            if (resp.find("\"exists\":true") != std::string::npos || 
                resp.find("\"exists\": true") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    bool uploadChunk(const std::string& video_path, const std::string& file_id,
                     int chunk_index, int total_chunks, const std::string& chunk_md5) {
    // 创建 socket
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1) {
        std::cerr << "Error creating socket" << std::endl;
        return false;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = inet_addr(server_ip.c_str());

    if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        std::cerr << "Error connecting to server" << std::endl;
        close(client_socket);
        return false;
    }

    std::cout << "Connected to server successfully!" << std::endl;

    std::ifstream file(video_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error opening video file: " << video_path << std::endl;
        close(client_socket);
        return false;
    }

    // 定位到分片位置
    const size_t CHUNK_SIZE = 5 * 1024 * 1024; // 5MB per chunk
    file.seekg(chunk_index * CHUNK_SIZE, std::ios::beg);
    
    // 读取分片数据
    std::vector<char> chunk_buffer(CHUNK_SIZE);
    file.read(chunk_buffer.data(), CHUNK_SIZE);
    size_t chunk_size = file.gcount();
    file.close();

    std::cout << "Uploading chunk " << chunk_index << "/" << total_chunks 
              << ", Size: " << chunk_size << " bytes" << std::endl;

    // === 构建 multipart/form-data 请求 ===
    std::string boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    std::ostringstream body_stream;

    // Part 1: 文件字段头
    body_stream << "--" << boundary << "\r\n";
    body_stream << "Content-Disposition: form-data; name=\"chunk\"; filename=\"chunk_" 
                << chunk_index << "\"\r\n";
    body_stream << "Content-Type: application/octet-stream\r\n";
    // 添加自定义头：文件 ID、分片索引、总分片数、分片 MD5
    body_stream << "X-File-ID: " << file_id << "\r\n";
    body_stream << "X-Chunk-Index: " << chunk_index << "\r\n";
    body_stream << "X-Total-Chunks: " << total_chunks << "\r\n";
    body_stream << "X-Chunk-MD5: " << chunk_md5 << "\r\n";
    body_stream << "\r\n";

    std::string body_start = body_stream.str();

    // Part 2: 结束边界
    std::string body_end = "\r\n--" + boundary + "--\r\n";

    // 计算总 Content-Length
    size_t total_body_size = body_start.length() + chunk_size + body_end.length();

    // 构建 HTTP 头部
    std::ostringstream header_stream;
    header_stream << "POST /upload/chunk HTTP/1.1\r\n";
    header_stream << "Host: " << server_ip << ":" << server_port << "\r\n";
    header_stream << "Content-Type: multipart/form-data; boundary=" << boundary << "\r\n";
    header_stream << "Content-Length: " << total_body_size << "\r\n";
    header_stream << "Connection: close\r\n";
    header_stream << "\r\n";

    std::string header = header_stream.str();

    // 发送 HTTP 头部
    if (send(client_socket, header.c_str(), header.length(), 0) == -1) {
        std::cerr << "Error sending HTTP header" << std::endl;
        close(client_socket);
        return false;
    }

    // 发送 multipart body 开始部分
    if (send(client_socket, body_start.c_str(), body_start.length(), 0) == -1) {
        std::cerr << "Error sending multipart start" << std::endl;
        close(client_socket);
        return false;
    }

    // 发送分片数据
    if (send(client_socket, chunk_buffer.data(), chunk_size, 0) == -1) {
        std::cerr << "Error sending chunk data" << std::endl;
        close(client_socket);
        return false;
    }

    // 发送结束边界
    if (send(client_socket, body_end.c_str(), body_end.length(), 0) == -1) {
        std::cerr << "Error sending multipart end boundary" << std::endl;
        close(client_socket);
        return false;
    }

    std::cout << "Chunk " << chunk_index << " sent, waiting for response..." << std::endl;

    // 接收服务器响应
    char response_buffer[4096];
    ssize_t bytes_received = recv(client_socket, response_buffer, sizeof(response_buffer)-1, 0);
    if (bytes_received > 0) {
        response_buffer[bytes_received] = '\0';
        std::string resp(response_buffer, bytes_received);
        std::cout << "Server response:\n" << resp << std::endl;
        
        // 检查是否成功
        if (resp.find("200 OK") == std::string::npos) {
            std::cerr << "Chunk upload failed, server returned error!" << std::endl;
            close(client_socket);
            return false;
        }
    } else {
        std::cerr << "No response from server!" << std::endl;
        close(client_socket);
        return false;
    }

    close(client_socket);
    std::cout << "✅ Chunk " << chunk_index << " uploaded successfully!" << std::endl;
    return true;
}

    // 发送完成请求
    bool sendCompleteRequest(const std::string& file_id, const std::string& original_filename, 
                             int total_chunks) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;

        struct sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(server_port);
        if (inet_pton(AF_INET, server_ip.c_str(), &serv_addr.sin_addr) <= 0) {
            close(sock);
            return false;
        }

        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            close(sock);
            return false;
        }

        // 构造 JSON body
        std::string json_body = "{"
            "\"file_id\":\"" + file_id + "\","
            "\"filename\":\"" + original_filename + "\","
            "\"total_chunks\":" + std::to_string(total_chunks) +
        "}";

        std::ostringstream req;
        req << "POST /upload/complete HTTP/1.1\r\n";
        req << "Host: " << server_ip << ":" << server_port << "\r\n";
        req << "Content-Type: application/json\r\n";
        req << "Content-Length: " << json_body.size() << "\r\n";
        req << "Connection: close\r\n\r\n";
        req << json_body;

        std::string request = req.str();
        send(sock, request.c_str(), request.size(), 0);

        char resp_buf[1024];
        ssize_t n = recv(sock, resp_buf, sizeof(resp_buf), 0);
        if (n > 0) {
            std::cout << "Complete response: " << std::string(resp_buf, n) << std::endl;
        }

        close(sock);
        return true;
    }
};

int main() {
    std::cout << "=== Chunked Video Upload Client (with MD5 & Redis) ===" << std::endl;
    
    std::string server_ip = "192.168.46.10";
    int server_port = 1316;
    std::string video_path = "../video_data/test_2.mp4";

    // 提取原始文件名
    size_t last_slash = video_path.find_last_of("/\\");
    std::string original_filename = (last_slash != std::string::npos) ? 
                                   video_path.substr(last_slash + 1) : video_path;

    // 1. 计算整个文件的 MD5 作为 FileID
    std::string file_md5 = MD5::hashFile(video_path);
    std::string file_id = "file_" + file_md5;
    
    std::cout << "File MD5: " << file_md5 << std::endl;
    std::cout << "File ID: " << file_id << std::endl;
    std::cout << "Original file: " << original_filename << std::endl;

    // 2. 计算分片信息
    const size_t CHUNK_SIZE = 5 * 1024 * 1024; // 5MB
    std::ifstream infile(video_path, std::ios::binary | std::ios::ate);
    size_t file_size = infile.tellg();
    infile.close();
    
    int total_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    std::cout << "File size: " << file_size << " bytes" << std::endl;
    std::cout << "Total chunks: " << total_chunks << std::endl;

    // 3. 上传每个分片
    VideoUploader uploader(server_ip, server_port);
    bool all_success = true;

    for (int i = 0; i < total_chunks; ++i) {
        std::cout << "\n--- Preparing chunk " << i << "/" << (total_chunks-1) << " ---" << std::endl;
        
        // 读取分片并计算 MD5
        std::ifstream chunk_file(video_path, std::ios::binary);
        chunk_file.seekg(i * CHUNK_SIZE, std::ios::beg);
        std::vector<char> chunk_buffer(CHUNK_SIZE);
        chunk_file.read(chunk_buffer.data(), CHUNK_SIZE);
        size_t chunk_size = chunk_file.gcount();
        chunk_file.close();
        
        std::string chunk_md5 = MD5::hashData(chunk_buffer.data(), chunk_size);
        std::cout << "Chunk " << i << " MD5: " << chunk_md5 << std::endl;
        
        // 上传分片（支持重试）
        int retry = 0;
        const int MAX_RETRY = 3;
        while (retry < MAX_RETRY) {
            if (uploader.uploadChunk(video_path, file_id, i, total_chunks, chunk_md5)) {
                break;
            }
            retry++;
            std::cout << "Retry " << retry << "/" << MAX_RETRY << " for chunk " << i << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        if (retry >= MAX_RETRY) {
            std::cerr << "Failed to upload chunk " << i << " after " << MAX_RETRY << " retries!" << std::endl;
            all_success = false;
            break;
        }
    }

    if (!all_success) {
        std::cout << "Upload failed! Aborting complete." << std::endl;
        return 1;
    }
    
    // 4. 发送完成请求
    std::cout << "\nSending /complete request..." << std::endl;
    if (uploader.sendCompleteRequest(file_id, original_filename, total_chunks)) {
        std::cout << "✅ Complete request sent successfully!" << std::endl;
    } else {
        std::cerr << "❌ Failed to send complete request!" << std::endl;
        return 1;
    }

    std::cout << "\n🎉 Upload completed successfully!" << std::endl;
    return 0;
}
