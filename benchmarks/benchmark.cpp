// RedisLite Benchmark
// Measures throughput (ops/sec) and latency (avg/p50/p99) for all
// major operations, and shows pipeline speedup vs single-shot.
//
// NOTE: This benchmark suite is AI-generated.
//       It is intended as a standalone performance tool and is NOT
//       part of the main RedisLite build or test suite.
//
// Build:  make          (inside this benchmarks/ directory)
// Run:    ./bin/redislite &  then  ./benchmark

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/ip.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

// ─── Timing ───────────────────────────────────────────────────────────────────

static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ─── Protocol constants ───────────────────────────────────────────────────────

enum { TAG_NIL=0, TAG_ERR=1, TAG_STR=2, TAG_INT=3, TAG_DBL=4, TAG_ARR=5 };

// ─── I/O ──────────────────────────────────────────────────────────────────────

static void io_write(int fd, const void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t rv = write(fd, (const char *)buf + done, n - done);
        if (rv <= 0) { perror("write"); exit(1); }
        done += rv;
    }
}

static void io_read(int fd, void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t rv = read(fd, (char *)buf + done, n - done);
        if (rv <= 0) { perror("read"); exit(1); }
        done += rv;
    }
}

// ─── Request encoding ─────────────────────────────────────────────────────────

static void send_req(int fd, const std::vector<std::string> &args) {
    uint32_t body = 4;
    for (auto &s : args) body += 4 + (uint32_t)s.size();

    std::vector<uint8_t> buf(4 + body);
    uint8_t *p = buf.data();
    memcpy(p, &body, 4); p += 4;
    uint32_t n = args.size();
    memcpy(p, &n, 4);    p += 4;
    for (auto &s : args) {
        uint32_t len = s.size();
        memcpy(p, &len, 4); p += 4;
        memcpy(p, s.data(), s.size()); p += s.size();
    }
    io_write(fd, buf.data(), buf.size());
}

// ─── Response draining (we just discard bytes, not parse) ─────────────────────

static void drain_resp(int fd) {
    uint8_t hdr[4];
    io_read(fd, hdr, 4);
    uint32_t len;
    memcpy(&len, hdr, 4);
    std::vector<uint8_t> body(len);
    if (len > 0) io_read(fd, body.data(), len);
}

// ─── Connection ───────────────────────────────────────────────────────────────

static int connect_server() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
            "Cannot connect to 127.0.0.1:1234 — is ../bin/redislite running?\n");
        exit(1);
    }
    return fd;
}

// ─── Stat helpers ─────────────────────────────────────────────────────────────

struct Stats {
    double ops_per_sec;
    double avg_us;
    double p50_us;
    double p99_us;
    double min_us;
    double max_us;
};

static Stats compute(std::vector<double> &latencies_us, double elapsed_sec, int ops) {
    std::sort(latencies_us.begin(), latencies_us.end());
    Stats s;
    s.ops_per_sec = ops / elapsed_sec;
    s.min_us      = latencies_us.front();
    s.max_us      = latencies_us.back();
    double sum = 0; for (double v : latencies_us) sum += v;
    s.avg_us  = sum / latencies_us.size();
    s.p50_us  = latencies_us[(size_t)(latencies_us.size() * 0.50)];
    s.p99_us  = latencies_us[(size_t)(latencies_us.size() * 0.99)];
    return s;
}

static void print_stats(const char *label, const Stats &s) {
    printf("  %-30s  %8.0f ops/s   avg=%5.1fµs  p50=%5.1fµs  p99=%6.1fµs\n",
           label, s.ops_per_sec, s.avg_us, s.p50_us, s.p99_us);
}

// ─── Benchmark: single-shot (one request → wait for response) ─────────────────

static Stats bench_single(int fd,
                           const std::vector<std::string> &req,
                           int ops)
{
    // warmup
    for (int i = 0; i < 200; i++) { send_req(fd, req); drain_resp(fd); }

    std::vector<double> lat;
    lat.reserve(ops);
    double t0 = now_sec();
    for (int i = 0; i < ops; i++) {
        double s = now_sec();
        send_req(fd, req);
        drain_resp(fd);
        lat.push_back((now_sec() - s) * 1e6);
    }
    double elapsed = now_sec() - t0;
    return compute(lat, elapsed, ops);
}

// ─── Benchmark: pipelined (send N, then drain N) ──────────────────────────────

static Stats bench_pipeline(int fd,
                             const std::vector<std::string> &req,
                             int pipe_depth, int total_ops)
{
    // warmup
    for (int i = 0; i < pipe_depth * 5; i++) send_req(fd, req);
    for (int i = 0; i < pipe_depth * 5; i++) drain_resp(fd);

    std::vector<double> lat;
    int batches = total_ops / pipe_depth;
    lat.reserve(batches);
    double t0 = now_sec();
    for (int b = 0; b < batches; b++) {
        double s = now_sec();
        for (int i = 0; i < pipe_depth; i++) send_req(fd, req);
        for (int i = 0; i < pipe_depth; i++) drain_resp(fd);
        lat.push_back((now_sec() - s) * 1e6 / pipe_depth); // per-op latency
    }
    double elapsed = now_sec() - t0;
    return compute(lat, elapsed, batches * pipe_depth);
}

// ─── Benchmark: ZSet zadd ──────────────────────────────────────────────────────

static Stats bench_zadd(int fd, int ops) {
    // warmup
    for (int i = 0; i < 200; i++) {
        char score[32], name[32];
        snprintf(score, sizeof(score), "%d", i);
        snprintf(name,  sizeof(name),  "m%d", i);
        send_req(fd, {"zadd", "bench_zset", score, name});
        drain_resp(fd);
    }

    std::vector<double> lat;
    lat.reserve(ops);
    double t0 = now_sec();
    for (int i = 0; i < ops; i++) {
        char score[32], name[32];
        snprintf(score, sizeof(score), "%d", i);
        snprintf(name,  sizeof(name),  "m%d", i);
        double s = now_sec();
        send_req(fd, {"zadd", "bench_zset", score, name});
        drain_resp(fd);
        lat.push_back((now_sec() - s) * 1e6);
    }
    double elapsed = now_sec() - t0;
    // cleanup
    send_req(fd, {"del", "bench_zset"});
    drain_resp(fd);
    return compute(lat, elapsed, ops);
}

// ─── Benchmark: ZSet zquery (range scan) ──────────────────────────────────────

static Stats bench_zquery(int fd, int ops) {
    // load 1000 members
    for (int i = 0; i < 1000; i++) {
        char score[32], name[32];
        snprintf(score, sizeof(score), "%d", i);
        snprintf(name,  sizeof(name),  "m%d", i);
        send_req(fd, {"zadd", "bench_zset", score, name});
        drain_resp(fd);
    }
    std::vector<std::string> qcmd = {"zquery","bench_zset","0","","0","50"};

    std::vector<double> lat;
    lat.reserve(ops);
    double t0 = now_sec();
    for (int i = 0; i < ops; i++) {
        double s = now_sec();
        send_req(fd, qcmd);
        drain_resp(fd);
        lat.push_back((now_sec() - s) * 1e6);
    }
    double elapsed = now_sec() - t0;
    send_req(fd, {"del", "bench_zset"});
    drain_resp(fd);
    return compute(lat, elapsed, ops);
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    printf("\n\033[1;36m━━━  RedisLite Benchmark  ━━━\033[0m\n");
    printf("Connecting to 127.0.0.1:1234...\n\n");

    int fd = connect_server();
    printf("Connected.\n\n");

    const int N       = 50000;   // ops for single-shot benches
    const int N_ZSET  = 20000;   // ops for zset (slightly slower)

    // ── Seed a fixed key for GET benchmarks ──────────────────────────────────
    send_req(fd, {"set", "bench_key", "hello_world"});
    drain_resp(fd);

    printf("\033[1m[ Single-shot throughput & latency  (N=%d) ]\033[0m\n", N);
    {
        auto s = bench_single(fd, {"set", "bench_key", "hello_world"}, N);
        print_stats("SET", s);

        auto g = bench_single(fd, {"get", "bench_key"}, N);
        print_stats("GET", g);

        auto d = bench_single(fd, {"del", "bench_key"}, N / 10);
        print_stats("DEL (N/10 samples)", d);
        // restore key
        send_req(fd, {"set", "bench_key", "hello_world"}); drain_resp(fd);

        auto k = bench_single(fd, {"keys"}, N / 5);
        print_stats("KEYS", k);
    }

    printf("\n\033[1m[ ZSet throughput & latency  (N=%d) ]\033[0m\n", N_ZSET);
    {
        auto za = bench_zadd(fd, N_ZSET);
        print_stats("ZADD (unique members)", za);

        auto zq = bench_zquery(fd, N_ZSET / 2);
        print_stats("ZQUERY (top-50 from 1k members)", zq);
    }

    printf("\n\033[1m[ Pipeline throughput  (N=%d per depth) ]\033[0m\n", N);
    Stats s1, s10, s100;
    {
        s1   = bench_pipeline(fd, {"set","bench_key","v"}, 1,   N);
        s10  = bench_pipeline(fd, {"set","bench_key","v"}, 10,  N);
        s100 = bench_pipeline(fd, {"set","bench_key","v"}, 100, N);
        print_stats("SET  pipeline depth=1",   s1);
        print_stats("SET  pipeline depth=10",  s10);
        print_stats("SET  pipeline depth=100", s100);
    }

    // ── Summary ───────────────────────────────────────────────────────────────
    double speedup10  = s10.ops_per_sec  / s1.ops_per_sec;
    double speedup100 = s100.ops_per_sec / s1.ops_per_sec;

    printf("\n\033[1m[ Pipeline speedup over single-shot ]\033[0m\n");
    printf("  depth=10  → \033[32m%.1fx\033[0m faster  (%+.0f%% throughput gain)\n",
           speedup10,  (speedup10  - 1.0) * 100.0);
    printf("  depth=100 → \033[32m%.1fx\033[0m faster  (%+.0f%% throughput gain)\n",
           speedup100, (speedup100 - 1.0) * 100.0);

    // Cleanup
    send_req(fd, {"del", "bench_key"}); drain_resp(fd);
    close(fd);

    printf("\n\033[1;32m✓ Benchmark complete.\033[0m\n\n");
    return 0;
}
