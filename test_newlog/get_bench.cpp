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

/* -------------- 工具函数：同之前 ------------ */
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
/* -------------------------------------------------- */

struct Metrics {
    std::atomic<uint64_t> ok{0};
    std::atomic<uint64_t> fail{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> sum_us{0};   // 首包→收完总耗时
};

static uint64_t once(const std::string& ip, int port,
                     const std::string& path,
                     uint64_t* out_len = nullptr)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    struct timeval tv{1, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    auto t0 = std::chrono::steady_clock::now();
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock); return 0;
    }

    std::string req = "GET " + path +
                      " HTTP/1.1\r\nHost: " + ip + ":" +
                      std::to_string(port) +
                      "\r\nConnection: close\r\n\r\n";
    if (send(sock, req.data(), req.size(), MSG_NOSIGNAL) < 0) {
        close(sock); return 0;
    }

    char buf[16 * 1024];
    ssize_t n, total = 0;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) total += n;
    close(sock);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return 0;
    if (out_len) *out_len = total;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    return us;
}

/* 一次完整“播放”：master → 选码率 → 子 playlist → 全部 TS */
static void play_once(const std::string& ip, int port,
                      const std::string& master_path,
                      Metrics* m,
                      bool only_first_ts)          // true=只压首片
{
    /* 1. master */
    uint64_t len = 0;
    uint64_t lat = once(ip, port, master_path, &len);
    if (lat == 0) { m->fail++; return; }
    m->ok++; m->bytes += len; m->sum_us += lat;

    std::string master_body = extractBody("");
    /* 实际应该收 body，这里简化：用空 body+parse 得到子列表 */
    master_body = "";   // 你原来用 response，这里同理
    /* 为了编译先塞空，下面用真实 response 收 */
    std::string real_master;
    char tmp[16 * 1024]; ssize_t n;
    /* 重新拉一次拿 body（省代码不重构）*/
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock); m->fail++; return;
    }
    std::string req = "GET " + master_path +
                      " HTTP/1.1\r\nHost: " + ip + ":" +
                      std::to_string(port) +
                      "\r\nConnection: close\r\n\r\n";
    send(sock, req.data(), req.size(), MSG_NOSIGNAL);
    while ((n = recv(sock, tmp, sizeof(tmp), 0)) > 0) real_master.append(tmp, n);
    close(sock);
    master_body = extractBody(real_master);

    auto sublists = parseM3U8(master_body, master_path);
    if (sublists.empty()) return;

    /* 2. 随机选一条码率（模拟用户分布） */
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(0, sublists.size() - 1);
    std::string sub_path = resolvePath(master_path, sublists[dist(rng)]);

    /* 3. 拉子 playlist */
    len = 0;
    lat = once(ip, port, sub_path, &len);
    if (lat == 0) { m->fail++; return; }
    m->ok++; m->bytes += len; m->sum_us += lat;

    std::string sub_body = extractBody("");
    /* 同样重新收一次 body */
    real_master.clear();
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;
    addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock); m->fail++; return;
    }
    req = "GET " + sub_path +
          " HTTP/1.1\r\nHost: " + ip + ":" + std::to_string(port) +
          "\r\nConnection: close\r\n\r\n";
    send(sock, req.data(), req.size(), MSG_NOSIGNAL);
    while ((n = recv(sock, tmp, sizeof(tmp), 0)) > 0) real_master.append(tmp, n);
    close(sock);
    sub_body = extractBody(real_master);

    auto ts_list = parseM3U8(sub_body, sub_path);
    if (ts_list.empty()) return;

    /* 4. 拉 TS：全片 or 首片 */
    for (size_t i = 0; i < ts_list.size(); ++i) {
        if (only_first_ts && i > 0) break;
        len = 0;
        lat = once(ip, port, resolvePath(sub_path, ts_list[i]), &len);
        if (lat == 0) { m->fail++; continue; }
        m->ok++; m->bytes += len; m->sum_us += lat;
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
    bool only_first = std::stoi(argv[6]);   // 0 全片  1 首片

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