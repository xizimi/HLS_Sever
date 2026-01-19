#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fstream>
#include <string>
#include <sstream>
#include <cstring>

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

int main() {
    std::cout << "=== TinyWebServer Video Upload Client ===" << std::endl;
    
    // 默认服务器配置
    std::string server_ip = "192.168.46.10";
    int server_port = 1316; // 默认端口

    
    std::cout << "Enter local video file path: ";
    std::string video_path="../video_data/2.mp4";
    
    // 提取文件名
    size_t last_slash = video_path.find_last_of("/\\");
    std::string filename = (last_slash != std::string::npos) ? 
                          video_path.substr(last_slash + 1) : video_path;

    VideoUploader uploader(server_ip, server_port);
    
    std::cout << "Starting upload process..." << std::endl;
    std::cout << "Server: " << server_ip << ":" << server_port << std::endl;
    std::cout << "File: " << video_path << std::endl;
    std::cout << "Remote filename: " << filename << std::endl;
    
    if (uploader.uploadVideo(video_path, filename)) {
        std::cout << "Upload successful!" << std::endl;
    } else {
        std::cout << "Upload failed!" << std::endl;
    }

    return 0;
}
