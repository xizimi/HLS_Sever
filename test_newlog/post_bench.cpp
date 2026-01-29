#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <cstring>

struct Metrics {
    std::atomic<uint64_t> ok{0};
    std::atomic<uint64_t> fail{0};
    std::atomic<uint64_t> bytes{0};
};

/* 发完直接关连接，不 recv */
bool once(const std::string& ip, int port,
          const std::string& file_path,
          const std::string& fake_name)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    /* 设 1 秒连接超时，防止线程死在 connect */
    struct timeval tv{1,0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock); return false;
    }

    /* 读文件大小 */
    std::ifstream fs(file_path, std::ios::binary | std::ios::ate);
    if (!fs) { close(sock); return false; }
    size_t fsize = fs.tellg();
    fs.seekg(0);
    std::string body(fsize, '\0');
    fs.read(&body[0], fsize);

    /* 构造 multipart 报文 */
    const std::string bound = "----BW";
    std::ostringstream head;
    head << "POST /upload HTTP/1.1\r\n"
         << "Host: " << ip << ":" << port << "\r\n"
         << "Content-Type: multipart/form-data; boundary=" << bound << "\r\n";
    std::string begin = "--" + bound +
        "\r\nContent-Disposition: form-data; name=\"video\"; filename=\"" +
        fake_name + "\"\r\nContent-Type: video/mp4\r\n\r\n";
    std::string end   = "\r\n--" + bound + "--\r\n";
    size_t total = begin.size() + fsize + end.size();
    head << "Content-Length: " << total << "\r\n\r\n";

    std::string hdr = head.str();
    /* 一次性写出（内核会拆包，无需我们循环） */
    if (send(sock, hdr.data(), hdr.size(), MSG_NOSIGNAL) < 0 ||
        send(sock, begin.data(), begin.size(), MSG_NOSIGNAL) < 0 ||
        send(sock, body.data(), body.size(), MSG_NOSIGNAL) < 0 ||
        send(sock, end.data(), end.size(), MSG_NOSIGNAL) < 0) {
        close(sock); return false;
    }
    /* ****** 关键：不等响应，立即关 ****** */
    close(sock);
    return true;
}

void worker(int tid, int duration_sec,
            const std::string& ip, int port,
            const std::string& file, Metrics* m)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
    size_t fsize = std::ifstream(file, std::ios::binary | std::ios::ate).tellg();
    uint64_t local_bytes = 0, local_ok = 0, local_fail = 0;

    while (std::chrono::steady_clock::now() < end) {
        bool ok = once(ip, port, file,
                       "bench_" + std::to_string(tid) +
                       "_" + std::to_string(local_ok) + ".mp4");
        if (ok) {
            ++local_ok;
            local_bytes += fsize;
        } else {
            ++local_fail;
        }
    }
    /* 聚合到全局原子变量 */
    m->ok   += local_ok;
    m->fail += local_fail;
    m->bytes += local_bytes;
}

int main(int argc, char* argv[]) {
    if (argc != 6) {
        std::cerr << "Usage: " << argv[0]
                  << " <ip> <port> <mp4> <threads> <seconds>\n";
        return 1;
    }
    const char* ip   = argv[1];
    int port       = std::stoi(argv[2]);
    const char* file = argv[3];
    int threads    = std::stoi(argv[4]);
    int seconds    = std::stoi(argv[5]);

    Metrics m;
    std::vector<std::thread> pool;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < threads; ++i)
        pool.emplace_back(worker, i, seconds, ip, port, file, &m);

    /* 实时打印 */
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto elapse = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - start).count();
        if (elapse >= seconds) break;
        uint64_t ok = m.ok.load(), fail = m.fail.load();
        double qps = (ok + fail) / (double)elapse;
        std::cout << "[" << elapse << "s] QPS=" << (int)qps
                  << " ok=" << ok << " fail=" << fail << "\n";
    }

    for (auto& t : pool) t.join();

    uint64_t ok = m.ok.load(), fail = m.fail.load();
    double thr_mbps = m.bytes.load() / 1024.0 / 1024.0 / seconds;
    std::cout << "\n===== No-Wait Result =====\n";
    std::cout << "Requests : " << (ok + fail) << " (ok=" << ok << " fail=" << fail << ")\n";
    std::cout << "QPS      : " << (int)((ok + fail) / (double)seconds) << "\n";
    std::cout << "Throughput: " << thr_mbps << " MB/s\n";
    return 0;
}