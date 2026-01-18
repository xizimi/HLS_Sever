// test_upload.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "192.168.46.10"
#define SERVER_PORT 1316
#define BOUNDARY "----WebKitFormBoundary7MA4YWxkTrZu0gW"

int main() {
    int sockfd;
    struct sockaddr_in server_addr;

    // === 1. 准备视频二进制数据（64字节合法MP4头）===
    unsigned char video_data[] = {
        0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p', 'm', 'p', '4', '2', 0x00, 0x00, 0x00, 0x00,
        'm', 'p', '4', '2', 'i', 's', 'o', 'm', 0x00, 0x00, 0x02, 0x00,
        'T', 'E', 'S', 'T', '_', 'V', 'I', 'D', 'E', 'O', '_', 'D', 'A', 'T', 'A', '_',
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'A', 'B', 'C', 'D', 'E', 'F',
        'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V'
    };
    size_t video_len = sizeof(video_data); // 64

    // === 2. 构造 multipart 各部分 ===
    const char* header_part =
        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"video\"; filename=\"test.mp4\"\r\n"
        "Content-Type: video/mp4\r\n"
        "\r\n";

    const char* footer_part = "\r\n--" BOUNDARY "--\r\n";

    size_t header_len = strlen(header_part);
    size_t footer_len = strlen(footer_part);
    size_t total_body_len = header_len + video_len + footer_len;

    // === 3. 分配并拼接完整 body ===
    unsigned char* full_body = (unsigned char*)malloc(total_body_len);
    if (!full_body) {
        perror("malloc");
        exit(1);
    }

    unsigned char* p = full_body;
    memcpy(p, header_part, header_len);
    p += header_len;
    memcpy(p, video_data, video_len);
    p += video_len;
    memcpy(p, footer_part, footer_len);

    printf("[+] Body size: %zu bytes (header=%zu, video=%zu, footer=%zu)\n",
           total_body_len, header_len, video_len, footer_len);

    // === 4. 创建 socket 并连接 ===
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        free(full_body);
        exit(1);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        free(full_body);
        close(sockfd);
        exit(1);
    }

    // === 5. 发送 HTTP 头 ===
    char request_header[512];
    int header_bytes = snprintf(request_header, sizeof(request_header),
        "POST /upload HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: multipart/form-data; boundary=" BOUNDARY "\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        SERVER_IP, SERVER_PORT, total_body_len
    );

    if (send(sockfd, request_header, header_bytes, 0) <= 0) {
        perror("send header");
        free(full_body);
        close(sockfd);
        exit(1);
    }
    printf("[+] Sent HTTP header (%d bytes)\n", header_bytes);

    // === 6. 发送 body（二进制安全）===
    if (send(sockfd, full_body, total_body_len, 0) <= 0) {
        perror("send body");
        free(full_body);
        close(sockfd);
        exit(1);
    }
    printf("[+] Sent body (%zu bytes)\n", total_body_len);

    // === 7. 接收响应 ===
    char response[2048];
    ssize_t n = recv(sockfd, response, sizeof(response) - 1, 0);
    if (n > 0) {
        response[n] = '\0';
        printf("\n[Server Response]\n%s\n", response);
    }

    free(full_body);
    close(sockfd);
    printf("[+] Done.\n");
    return 0;
}