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

class VideoUploader {
private:
    std::string server_ip;
    int server_port;

public:
    VideoUploader(const std::string& ip, int port) : server_ip(ip), server_port(port) {}

    bool uploadVideo(const std::string& video_path, const std::string& remote_filename) {
    // 创建socket
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

    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::cout << "Uploading video file: " << video_path << ", Size: " << file_size << " bytes" << std::endl;

    // === 构建 multipart/form-data 请求 ===
    std::string boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW"; // 模拟浏览器
    std::ostringstream body_stream;

    // Part 1: 文件字段头
    body_stream << "--" << boundary << "\r\n";
    body_stream << "Content-Disposition: form-data; name=\"video\"; filename=\"" << remote_filename << "\"\r\n";
    body_stream << "Content-Type: video/mp4\r\n\r\n"; // 可扩展为根据扩展名判断

    std::string body_start = body_stream.str();

    // Part 2: 结束边界
    std::string body_end = "\r\n--" + boundary + "--\r\n";

    // 计算总 Content-Length
    size_t total_body_size = body_start.length() + file_size + body_end.length();

    // 构建 HTTP 头部
    std::ostringstream header_stream;
    header_stream << "POST /upload HTTP/1.1\r\n";
    header_stream << "Host: " << server_ip << ":" << server_port << "\r\n";
    header_stream << "Content-Type: multipart/form-data; boundary=" << boundary << "\r\n";
    header_stream << "Content-Length: " << total_body_size << "\r\n";
    header_stream << "Connection: close\r\n";
    header_stream << "\r\n";

    std::string header = header_stream.str();

    std::cout << header << std::endl;
    // 发送 HTTP 头部
    if (send(client_socket, header.c_str(), header.length(), 0) == -1) {
        std::cerr << "Error sending HTTP header" << std::endl;
        file.close();
        close(client_socket);
        return false;
    }

    std::cout << body_start << std::endl;
    // 发送 multipart body 开始部分
    if (send(client_socket, body_start.c_str(), body_start.length(), 0) == -1) {
        std::cerr << "Error sending multipart start" << std::endl;
        file.close();
        close(client_socket);
        return false;
    }

    // 分块发送文件数据
    const size_t buffer_size = 8192;
    char* buffer = new char[buffer_size];
    size_t total_sent = body_start.length(); // 已发送的字节数（用于进度显示）

    while (file.good()) {
        file.read(buffer, buffer_size);
        size_t read_bytes = file.gcount();

        if (read_bytes > 0) {
            ssize_t sent = send(client_socket, buffer, read_bytes, 0);
            if (sent == -1) {
                std::cerr << "Error sending file data" << std::endl;
                delete[] buffer;
                file.close();
                close(client_socket);
                return false;
            }
            total_sent += sent;

            // 显示进度（基于整个请求体）
            double progress = (double)total_sent / (body_start.length() + file_size) * 100;
            std::cout << "\rProgress: " << progress << "% (" 
                      << total_sent << "/" << (body_start.length() + file_size) << " bytes)";
            std::cout.flush();
        }

        if (read_bytes < buffer_size) break;
    }

    // 发送结束边界
    if (send(client_socket, body_end.c_str(), body_end.length(), 0) == -1) {
        std::cerr << "Error sending multipart end boundary" << std::endl;
        delete[] buffer;
        file.close();
        close(client_socket);
        return false;
    }

    total_sent += body_end.length();
    std::cout << std::endl;
    std::cout << "Upload completed! Total request size: " << total_sent << " bytes" << std::endl;

    delete[] buffer;
    file.close();

    // 接收服务器响应
    // char response_buffer[4096];
    // ssize_t bytes_received = recv(client_socket, response_buffer, sizeof(response_buffer)-1, 0);
    // if (bytes_received > 0) {
    //     response_buffer[bytes_received] = '\0';
    //     std::cout << "Server response:\n" << std::string(response_buffer, bytes_received) << std::endl;
    // }

    close(client_socket);
    // 在uploadVideo()函数末尾
    size_t header_len = header.length();
    size_t total_request_bytes = header_len + total_sent;

    std::cout << "\n=== Final Transmission Report ===" << std::endl;
    std::cout << "Header size:          " << header_len << " bytes" << std::endl;
    std::cout << "Body start size:      " << body_start.length() << " bytes" << std::endl;
    std::cout << "File data size:       " << file_size << " bytes" << std::endl;
    std::cout << "Body end size:        " << body_end.length() << " bytes" << std::endl;
    std::cout << "Total body size:      " << total_body_size << " bytes (should match Content-Length)" << std::endl;
    std::cout << "Actually sent in body:" << total_sent << " bytes" << std::endl;
    std::cout << "----------------------------------------" << std::endl;
    std::cout << "✅ TOTAL REQUEST SIZE: " << total_request_bytes << " bytes" << std::endl;
    return true;
}
};
std::vector<std::string> splitFileIntoChunks(const std::string& input_path, size_t chunk_size = 5 * 1024 * 1024) {
    std::ifstream infile(input_path, std::ios::binary);
    if (!infile.is_open()) {
        std::cerr << "Cannot open input file: " << input_path << std::endl;
        return {};
    }

    infile.seekg(0, std::ios::end);
    size_t file_size = infile.tellg();
    infile.seekg(0, std::ios::beg);

    size_t total_chunks = (file_size + chunk_size - 1) / chunk_size;
    std::vector<std::string> chunk_files;

    std::cout << "Splitting " << input_path << " (" << file_size << " bytes) into " 
              << total_chunks << " chunks..." << std::endl;

    std::vector<char> buffer(chunk_size);
    for (size_t i = 0; i < total_chunks; ++i) {
        size_t bytes_to_read = (i == total_chunks - 1) ? (file_size - i * chunk_size) : chunk_size;
        infile.read(buffer.data(), bytes_to_read);

        // 生成临时 chunk 文件名
        std::string chunk_name = "temp_chunk_" + std::to_string(i);
        std::ofstream outfile(chunk_name, std::ios::binary);
        if (!outfile.is_open()) {
            std::cerr << "Cannot create chunk file: " << chunk_name << std::endl;
            // 清理已创建的 chunks
            for (const auto& f : chunk_files) std::remove(f.c_str());
            return {};
        }
        outfile.write(buffer.data(), bytes_to_read);
        outfile.close();

        chunk_files.push_back(chunk_name);
        std::cout << "Created: " << chunk_name << " (" << bytes_to_read << " bytes)" << std::endl;
    }

    infile.close();
    return chunk_files;
}

// ====== 新增：发送 /complete 请求 ======
bool sendCompleteRequest(const std::string& server_ip, int server_port,
                         const std::string& upload_id, const std::string& final_filename,size_t total_chunks) {
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
        "\"upload_id\":\"" + upload_id + "\","
        "\"filename\":\"" + final_filename + "\","
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
    recv(sock, resp_buf, sizeof(resp_buf), 0); // 简单读响应

    close(sock);
    return true;
}
int main() {
       std::cout << "=== Chunked Video Upload Client ===" << std::endl;
    
    std::string server_ip = "192.168.46.10";
    int server_port = 1316;
    std::string video_path = "../video_data/test_2.mp4";

    // 提取原始文件名
    size_t last_slash = video_path.find_last_of("/\\");
    std::string original_filename = (last_slash != std::string::npos) ? 
                                   video_path.substr(last_slash + 1) : video_path;

    // 生成 upload_id（必须和服务端一致）
    srand(time(nullptr));
    std::string upload_id = "vid_" + std::to_string(time(nullptr)) + "_" + std::to_string(rand() % 10000);

    std::cout << "Upload ID: " << upload_id << std::endl;
    std::cout << "Original file: " << original_filename << std::endl;

    // 1. 拆分文件
    std::vector<std::string> chunks = splitFileIntoChunks(video_path);
    if (chunks.empty()) {
        std::cerr << "Failed to split file!" << std::endl;
        return 1;
    }

    // 2. 上传每个 chunk（复用你的 uploadVideo）
    VideoUploader uploader(server_ip, server_port);
    bool all_success = true;

    for (size_t i = 0; i < chunks.size(); ++i) {
        std::cout << "\n--- Uploading chunk " << i << "/" << (chunks.size()-1) << " ---" << std::endl;
        
        // 构造带 upload_id 和 index 的“假文件名”，让服务端能识别
        // 例如: "vid_12345_6789_chunk_0"
        std::string fake_filename = upload_id +"/"+ "chunk_" + std::to_string(i);

        if (!uploader.uploadVideo(chunks[i], fake_filename)) {
            std::cerr << "Failed to upload chunk " << i << std::endl;
            all_success = false;
            break;
        }
    }

    // 3. 清理临时 chunk 文件
    for (const auto& chunk : chunks) {
        std::remove(chunk.c_str());
    }

    if (!all_success) {
        std::cout << "Upload failed! Aborting complete." << std::endl;
        return 1;
    }
    std::cout << "\nWaiting for server to finish processing chunks..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(10));
    //4. 发送 complete
    std::cout << "\nSending /complete request..." << std::endl;
    if (sendCompleteRequest(server_ip, server_port, upload_id, original_filename,chunks.size())) {
        std::cout << "✅ Complete request sent successfully!" << std::endl;
    } else {
        std::cerr << "❌ Failed to send complete request!" << std::endl;
        return 1;
    }

    return 0;
}
