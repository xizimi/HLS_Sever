#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <cstring>

/* ---------------- 工具函数 ---------------- */
static std::string extractBody(const std::string& resp)
{
    size_t p = resp.find("\r\n\r\n");
    return (p == std::string::npos) ? resp : resp.substr(p + 4);
}

static std::vector<std::string> parseM3U8(const std::string& body,
                                          const std::string& base)
{
    std::vector<std::string> seg;
    std::istringstream ss(body);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        seg.push_back(line);   // 可能是子 playlist 或 ts
    }
    return seg;
}

static std::string resolvePath(const std::string& base,
                               const std::string& rel)
{
    if (rel.empty() || rel[0] == '/') return rel;
    size_t last = base.find_last_of('/');
    return (last == std::string::npos) ? ("/" + rel)
                                       : base.substr(0, last + 1) + rel;
}
/* ---------------------------------------- */

struct Metrics {
    std::atomic<uint64_t> ok{0};
    std::atomic<uint64_t> fail{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> sum_us{0};
};

/* 一次 GET：返回 <body, 总字节, 耗时μs>，失败返回空三元组 */
static std::tuple<std::string, uint64_t, uint64_t>
once(const std::string& ip, int port, const std::string& path)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return {"", 0, 0};

    struct timeval tv{1, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    auto t0 = std::chrono::steady_clock::now();
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock); return {"", 0, 0};
    }

    std::string req = "GET " + path +
                      " HTTP/1.1\r\nHost: " + ip + ":" +
                      std::to_string(port) +
                      "\r\nConnection: close\r\n\r\n";
    if (send(sock, req.data(), req.size(), MSG_NOSIGNAL) < 0) {
        close(sock); return {"", 0, 0};
    }

    char buf[16 * 1024];
    ssize_t n;
    std::string raw;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) raw.append(buf, n);
    close(sock);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return {"", 0, 0};

    uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0).count();
    return {extractBody(raw), raw.size(), us};
}

/* 一次完整播放链路：master→子 playlist→TS（全片/首片） */
static void play_once(const std::string& ip, int port,
                      const std::string& master_path,
                      Metrics* m,
                      bool only_first_ts)
{
    /* ① master playlist */
    auto [mas_body, mas_len, mas_us] = once(ip, port, master_path);
    if (mas_body.empty()) { m->fail++; return; }
    m->ok++; m->bytes += mas_len; m->sum_us += mas_us;

    auto sublists = parseM3U8(mas_body, master_path);
    if (sublists.empty()) return;

    /* ② 随机选一条码率子 playlist */
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(0, sublists.size() - 1);
    std::string sub_path = resolvePath(master_path, sublists[dist(rng)]);

    /* ③ 子 playlist */
    auto [sub_body, sub_len, sub_us] = once(ip, port, sub_path);
    if (sub_body.empty()) { m->fail++; return; }
    m->ok++; m->bytes += sub_len; m->sum_us += sub_us;

    auto ts_list = parseM3U8(sub_body, sub_path);
    if (ts_list.empty()) return;

    /* ④ TS：全片 or 首片 */
    for (size_t i = 0; i < ts_list.size(); ++i) {
        if (only_first_ts && i > 0) break;
        auto [ts_body, ts_len, ts_us] = once(ip, port, resolvePath(sub_path, ts_list[i]));
        if (ts_body.empty()) { m->fail++; continue; }
        m->ok++; m->bytes += ts_len; m->sum_us += ts_us;
    }
}

static void worker(int tid, int duration_sec,
                   const std::string& ip, int port,
                   const std::string& master_path,
                   Metrics* m, bool only_first)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
    while (std::chrono::steady_clock::now() < end) {
        play_once(ip, port, master_path, m, only_first);
    }
}

/* ---------------- main ---------------- */
int main(int argc, char* argv[]) {
    if (argc != 7) {
        std::cerr << "Usage: " << argv[0]
                  << " <ip> <port> <master.m3u8_path> <threads> <seconds> <0|1>\n"
                  << "  last arg: 0=full play  1=only first ts (首帧)\n"
                  << "Example: ./get_bench_multi 192.168.46.10 1316 "
                     "/video/vid123/master.m3u8 50 60 0\n";
        return 1;
    }
    const char* ip   = argv[1];
    int port       = std::stoi(argv[2]);
    const char* path = argv[3];
    int threads    = std::stoi(argv[4]);
    int seconds    = std::stoi(argv[5]);
    bool only_first = std::stoi(argv[6]);

    Metrics m;
    std::vector<std::thread> pool;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < threads; ++i)
        pool.emplace_back(worker, i, seconds, ip, port, path, &m, only_first);

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto elapse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start).count();
        if (elapse_ms >= seconds * 1000) break;
        uint64_t ok = m.ok.load(), fail = m.fail.load();
        double qps = (ok + fail) * 1000.0 / elapse_ms;
        std::cout << "[" << (elapse_ms / 1000) << "."
                  << (elapse_ms % 1000 / 100)
                  << "s] QPS=" << (int)qps
                  << " ok=" << ok << " fail=" << fail << "\n";
    }

    for (auto& t : pool) t.join();

    uint64_t ok = m.ok.load(), fail = m.fail.load();
    double thr_mbps = m.bytes.load() / 1024.0 / 1024.0 / seconds;
    double avg_lat  = ok ? (double)m.sum_us.load() / ok / 1000.0 : 0;
    std::cout << "\n========== GET Multi-Rate Bench ==========\n";
    std::cout << "Requests : " << (ok + fail) << " (ok=" << ok << " fail=" << fail << ")\n";
    std::cout << "QPS      : " << (int)((ok + fail) / (double)seconds) << "\n";
    std::cout << "Throughput: " << thr_mbps << " MB/s\n";
    std::cout << "Avg latency: " << avg_lat << " ms\n";
    std::cout << "Mode: " << (only_first ? "首片" : "全片") << "\n";
    return 0;
}