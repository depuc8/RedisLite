// RedisLite Test Driver
// Connect to localhost:1234 and test every command.
// Run the server first:  ./bin/redislite
// Then run tests:        ./bin/test_client

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/ip.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

// ─── Protocol constants ────────────────────────────────────────────────────────

enum {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_STR = 2,
    TAG_INT = 3,
    TAG_DBL = 4,
    TAG_ARR = 5,
};

enum {
    ERR_UNKNOWN = 1,
    ERR_TOO_BIG = 2,
    ERR_BAD_TYP = 3,
    ERR_BAD_ARG = 4,
};

// ─── Test state ────────────────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

static void pass(const char *name) {
    printf("  \033[32m[PASS]\033[0m %s\n", name);
    g_pass++;
}

static void fail(const char *name, const char *reason = "") {
    printf("  \033[31m[FAIL]\033[0m %s  —  %s\n", name, reason);
    g_fail++;
}

// ─── Low-level I/O helpers ─────────────────────────────────────────────────────

static int32_t read_full(int fd, uint8_t *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t rv = read(fd, buf + done, n - done);
        if (rv <= 0) return -1;
        done += (size_t)rv;
    }
    return 0;
}

static int32_t write_full(int fd, const uint8_t *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t rv = write(fd, buf + done, n - done);
        if (rv <= 0) return -1;
        done += (size_t)rv;
    }
    return 0;
}

// ─── Request encoding ──────────────────────────────────────────────────────────
// Wire format: [4B total-body-len][4B nstr]([4B len][data])...

static void send_req(int fd, const std::vector<std::string> &cmd) {
    // calculate body size
    uint32_t body_len = 4; // nstr
    for (auto &s : cmd) body_len += 4 + (uint32_t)s.size();

    uint8_t buf[4 + body_len];
    uint8_t *p = buf;

    // message header
    memcpy(p, &body_len, 4); p += 4;
    // nstr
    uint32_t nstr = (uint32_t)cmd.size();
    memcpy(p, &nstr, 4); p += 4;
    // each string
    for (auto &s : cmd) {
        uint32_t len = (uint32_t)s.size();
        memcpy(p, &len, 4); p += 4;
        memcpy(p, s.data(), s.size()); p += s.size();
    }

    int32_t rv = write_full(fd, buf, sizeof(buf));
    if (rv < 0) {
        fprintf(stderr, "write_full failed: %s\n", strerror(errno));
        exit(1);
    }
}

// ─── Response decoding ─────────────────────────────────────────────────────────

struct Response {
    uint8_t  tag  = 0;
    int64_t  ival = 0;
    double   dval = 0.0;
    uint32_t ecode = 0;
    std::string str;
    std::vector<Response> arr;
};

static Response read_one_value(const uint8_t *&p, const uint8_t *end) {
    Response r;
    if (p >= end) { r.tag = 0xFF; return r; } // truncated
    r.tag = *p++;

    switch (r.tag) {
    case TAG_NIL:
        break;
    case TAG_ERR: {
        if (p + 8 > end) { r.tag = 0xFF; break; }
        memcpy(&r.ecode, p, 4); p += 4;
        uint32_t len; memcpy(&len, p, 4); p += 4;
        if (p + len > end) { r.tag = 0xFF; break; }
        r.str.assign((char *)p, len); p += len;
        break;
    }
    case TAG_STR: {
        if (p + 4 > end) { r.tag = 0xFF; break; }
        uint32_t len; memcpy(&len, p, 4); p += 4;
        if (p + len > end) { r.tag = 0xFF; break; }
        r.str.assign((char *)p, len); p += len;
        break;
    }
    case TAG_INT:
        if (p + 8 > end) { r.tag = 0xFF; break; }
        memcpy(&r.ival, p, 8); p += 8;
        break;
    case TAG_DBL:
        if (p + 8 > end) { r.tag = 0xFF; break; }
        memcpy(&r.dval, p, 8); p += 8;
        break;
    case TAG_ARR: {
        if (p + 4 > end) { r.tag = 0xFF; break; }
        uint32_t n; memcpy(&n, p, 4); p += 4;
        for (uint32_t i = 0; i < n; i++) {
            r.arr.push_back(read_one_value(p, end));
        }
        break;
    }
    default:
        r.tag = 0xFF; // unknown
    }
    return r;
}

static Response recv_resp(int fd) {
    // read 4-byte length header
    uint8_t header[4];
    if (read_full(fd, header, 4) < 0) {
        fprintf(stderr, "read header failed\n");
        exit(1);
    }
    uint32_t len;
    memcpy(&len, header, 4);

    // read body
    std::vector<uint8_t> body(len);
    if (len > 0 && read_full(fd, body.data(), len) < 0) {
        fprintf(stderr, "read body failed\n");
        exit(1);
    }

    const uint8_t *p   = body.data();
    const uint8_t *end = body.data() + len;
    return read_one_value(p, end);
}

// ─── Helpers: send + receive in one go ────────────────────────────────────────

static Response cmd(int fd, std::vector<std::string> args) {
    send_req(fd, args);
    return recv_resp(fd);
}

// ─── Assertion helpers ─────────────────────────────────────────────────────────

static void expect_nil(const char *name, Response &r) {
    if (r.tag == TAG_NIL) pass(name);
    else fail(name, "expected NIL");
}

static void expect_int(const char *name, Response &r, int64_t val) {
    if (r.tag == TAG_INT && r.ival == val) pass(name);
    else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected INT %lld, got tag=%d ival=%lld",
                 (long long)val, r.tag, (long long)r.ival);
        fail(name, buf);
    }
}

static void expect_str(const char *name, Response &r, const char *val) {
    if (r.tag == TAG_STR && r.str == val) pass(name);
    else {
        char buf[256];
        snprintf(buf, sizeof(buf), "expected STR '%s', got tag=%d str='%s'",
                 val, r.tag, r.str.c_str());
        fail(name, buf);
    }
}

static void expect_dbl(const char *name, Response &r, double val) {
    if (r.tag == TAG_DBL && r.dval == val) pass(name);
    else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected DBL %f, got tag=%d dval=%f",
                 val, r.tag, r.dval);
        fail(name, buf);
    }
}

static void expect_err(const char *name, Response &r, uint32_t code) {
    if (r.tag == TAG_ERR && r.ecode == code) pass(name);
    else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected ERR code %u, got tag=%d ecode=%u",
                 code, r.tag, r.ecode);
        fail(name, buf);
    }
}

static void expect_arr_size(const char *name, Response &r, size_t n) {
    if (r.tag == TAG_ARR && r.arr.size() == n) pass(name);
    else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected ARR[%zu], got tag=%d size=%zu",
                 n, r.tag, r.arr.size());
        fail(name, buf);
    }
}

// ─── Connection ───────────────────────────────────────────────────────────────

static int connect_to_server() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
            "\033[31mCannot connect to server at 127.0.0.1:1234\033[0m\n"
            "Make sure the server is running:  ./bin/redislite\n");
        exit(1);
    }
    return fd;
}

// ─── Test suites ──────────────────────────────────────────────────────────────

static void test_string_ops(int fd) {
    printf("\n\033[1m[ String operations ]\033[0m\n");
    Response r;

    // SET returns NIL on success
    r = cmd(fd, {"set", "foo", "bar"});
    expect_nil("set foo bar -> NIL", r);

    // GET existing key
    r = cmd(fd, {"get", "foo"});
    expect_str("get foo -> 'bar'", r, "bar");

    // GET non-existent key
    r = cmd(fd, {"get", "nosuchkey"});
    expect_nil("get missing key -> NIL", r);

    // SET overwrites existing key
    r = cmd(fd, {"set", "foo", "baz"});
    expect_nil("set foo baz (overwrite) -> NIL", r);
    r = cmd(fd, {"get", "foo"});
    expect_str("get foo after overwrite -> 'baz'", r, "baz");

    // DEL existing key
    r = cmd(fd, {"del", "foo"});
    expect_int("del foo -> 1", r, 1);

    // GET after DEL
    r = cmd(fd, {"get", "foo"});
    expect_nil("get foo after del -> NIL", r);

    // DEL non-existent key
    r = cmd(fd, {"del", "nosuchkey"});
    expect_int("del missing key -> 0", r, 0);

    // KEYS
    r = cmd(fd, {"set", "k1", "v1"});
    r = cmd(fd, {"set", "k2", "v2"});
    r = cmd(fd, {"set", "k3", "v3"});
    r = cmd(fd, {"keys"});
    if (r.tag == TAG_ARR && r.arr.size() == 3) pass("keys returns 3 entries");
    else {
        char buf[64]; snprintf(buf, sizeof(buf), "expected 3 keys, got %zu", r.arr.size());
        fail("keys returns 3 entries", buf);
    }
    cmd(fd, {"del", "k1"}); cmd(fd, {"del", "k2"}); cmd(fd, {"del", "k3"});
}

static void test_ttl(int fd) {
    printf("\n\033[1m[ TTL operations ]\033[0m\n");
    Response r;

    // Set up a key
    cmd(fd, {"set", "ttlkey", "hello"});

    // PTTL on key with no TTL
    r = cmd(fd, {"pttl", "ttlkey"});
    expect_int("pttl key with no TTL -> -1", r, -1);

    // PTTL on non-existent key
    r = cmd(fd, {"pttl", "ghost"});
    expect_int("pttl missing key -> -2", r, -2);

    // PEXPIRE
    r = cmd(fd, {"pexpire", "ttlkey", "500"});
    expect_int("pexpire ttlkey 500ms -> 1", r, 1);

    // PEXPIRE on missing key
    r = cmd(fd, {"pexpire", "ghost", "500"});
    expect_int("pexpire missing key -> 0", r, 0);

    // PTTL should return a positive value shortly after pexpire
    r = cmd(fd, {"pttl", "ttlkey"});
    if (r.tag == TAG_INT && r.ival > 0 && r.ival <= 500)
        pass("pttl after pexpire -> positive value <= 500");
    else {
        char buf[64]; snprintf(buf, sizeof(buf), "expected 0<ttl<=500, got %lld", (long long)r.ival);
        fail("pttl after pexpire -> positive value <= 500", buf);
    }

    // Wait for expiry
    printf("    (waiting 600ms for TTL expiry...)\n");
    usleep(600 * 1000);

    // Key should be gone
    r = cmd(fd, {"get", "ttlkey"});
    expect_nil("get expired key -> NIL", r);

    r = cmd(fd, {"pttl", "ttlkey"});
    expect_int("pttl expired key -> -2", r, -2);

    // Bad argument to PEXPIRE
    r = cmd(fd, {"set", "x", "1"});
    r = cmd(fd, {"pexpire", "x", "notanumber"});
    expect_err("pexpire bad arg -> ERR_BAD_ARG", r, ERR_BAD_ARG);
    cmd(fd, {"del", "x"});
}

static void test_sorted_set(int fd) {
    printf("\n\033[1m[ Sorted set (ZSet) operations ]\033[0m\n");
    Response r;

    // ZADD new members
    r = cmd(fd, {"zadd", "scores", "1.0", "alice"});
    expect_int("zadd alice 1.0 -> 1 (added)", r, 1);

    r = cmd(fd, {"zadd", "scores", "3.0", "bob"});
    expect_int("zadd bob 3.0 -> 1 (added)", r, 1);

    r = cmd(fd, {"zadd", "scores", "2.0", "charlie"});
    expect_int("zadd charlie 2.0 -> 1 (added)", r, 1);

    // ZSCORE existing member
    r = cmd(fd, {"zscore", "scores", "alice"});
    expect_dbl("zscore alice -> 1.0", r, 1.0);

    r = cmd(fd, {"zscore", "scores", "bob"});
    expect_dbl("zscore bob -> 3.0", r, 3.0);

    // ZSCORE non-existent member
    r = cmd(fd, {"zscore", "scores", "nobody"});
    expect_nil("zscore missing member -> NIL", r);

    // ZADD update (same key)
    r = cmd(fd, {"zadd", "scores", "5.0", "alice"});
    expect_int("zadd alice 5.0 (update) -> 0", r, 0);

    r = cmd(fd, {"zscore", "scores", "alice"});
    expect_dbl("zscore alice after update -> 5.0", r, 5.0);

    // ZREM existing member
    r = cmd(fd, {"zrem", "scores", "charlie"});
    expect_int("zrem charlie -> 1", r, 1);

    r = cmd(fd, {"zscore", "scores", "charlie"});
    expect_nil("zscore removed member -> NIL", r);

    // ZREM non-existent member
    r = cmd(fd, {"zrem", "scores", "nobody"});
    expect_int("zrem missing member -> 0", r, 0);

    // ZQUERY: seek >= (score=2.0, name=""), offset=0, limit=10
    // Remaining: bob(3.0), alice(5.0)
    r = cmd(fd, {"zquery", "scores", "2.0", "", "0", "10"});
    // Returns array of [name, score, name, score, ...]
    if (r.tag == TAG_ARR && r.arr.size() == 4 &&
        r.arr[0].tag == TAG_STR && r.arr[0].str == "bob" &&
        r.arr[1].tag == TAG_DBL && r.arr[1].dval == 3.0 &&
        r.arr[2].tag == TAG_STR && r.arr[2].str == "alice" &&
        r.arr[3].tag == TAG_DBL && r.arr[3].dval == 5.0)
    {
        pass("zquery score>=2.0 -> [bob:3.0, alice:5.0]");
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "tag=%d arr.size=%zu", r.tag, r.arr.size());
        fail("zquery score>=2.0 -> [bob:3.0, alice:5.0]", buf);
    }

    // ZQUERY with offset
    r = cmd(fd, {"zquery", "scores", "0.0", "", "1", "10"});
    // All: bob(3.0), alice(5.0); offset=1 skips bob
    if (r.tag == TAG_ARR && r.arr.size() == 2 &&
        r.arr[0].str == "alice" && r.arr[1].dval == 5.0)
    {
        pass("zquery with offset=1 -> [alice:5.0]");
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "tag=%d arr.size=%zu", r.tag, r.arr.size());
        fail("zquery with offset=1 -> [alice:5.0]", buf);
    }

    // ZQUERY with limit
    r = cmd(fd, {"zquery", "scores", "0.0", "", "0", "2"});
    // limit=2 means 1 entry (name+score pair = 2 items)
    if (r.tag == TAG_ARR && r.arr.size() == 2 && r.arr[0].str == "bob")
        pass("zquery with limit=2 -> [bob:3.0]");
    else {
        char buf[128];
        snprintf(buf, sizeof(buf), "tag=%d arr.size=%zu", r.tag, r.arr.size());
        fail("zquery with limit=2 -> [bob:3.0]", buf);
    }

    // ZQUERY with limit=0
    r = cmd(fd, {"zquery", "scores", "0.0", "", "0", "0"});
    expect_arr_size("zquery limit=0 -> empty array", r, 0);

    // ZSCORE on non-existent zset (treated as empty)
    r = cmd(fd, {"zscore", "nosuchzset", "x"});
    expect_nil("zscore on non-existent zset -> NIL", r);

    // Cleanup
    cmd(fd, {"del", "scores"});
}

static void test_type_errors(int fd) {
    printf("\n\033[1m[ Type error handling ]\033[0m\n");
    Response r;

    // Create a string key and a zset key
    cmd(fd, {"set", "strkey", "hello"});
    cmd(fd, {"zadd", "zkey", "1.0", "member"});

    // GET on a ZSet key
    r = cmd(fd, {"get", "zkey"});
    expect_err("get on zset key -> ERR_BAD_TYP", r, ERR_BAD_TYP);

    // SET on a ZSet key
    r = cmd(fd, {"set", "zkey", "value"});
    expect_err("set on zset key -> ERR_BAD_TYP", r, ERR_BAD_TYP);

    // ZADD on a string key
    r = cmd(fd, {"zadd", "strkey", "1.0", "x"});
    expect_err("zadd on string key -> ERR_BAD_TYP", r, ERR_BAD_TYP);

    // ZADD with non-numeric score
    r = cmd(fd, {"zadd", "zkey", "notafloat", "x"});
    expect_err("zadd with bad score -> ERR_BAD_ARG", r, ERR_BAD_ARG);

    // Unknown command
    r = cmd(fd, {"foobar", "x"});
    expect_err("unknown command -> ERR_UNKNOWN", r, ERR_UNKNOWN);

    // Cleanup
    cmd(fd, {"del", "strkey"});
    cmd(fd, {"del", "zkey"});
}

static void test_pipelining(int fd) {
    printf("\n\033[1m[ Pipelining ]\033[0m\n");

    // Send 5 requests before reading any response
    send_req(fd, {"set", "p1", "a"});
    send_req(fd, {"set", "p2", "b"});
    send_req(fd, {"set", "p3", "c"});
    send_req(fd, {"get", "p1"});
    send_req(fd, {"get", "p2"});

    // Now read all 5 responses
    Response r0 = recv_resp(fd);
    Response r1 = recv_resp(fd);
    Response r2 = recv_resp(fd);
    Response r3 = recv_resp(fd);
    Response r4 = recv_resp(fd);

    bool ok = (r0.tag == TAG_NIL) &&
              (r1.tag == TAG_NIL) &&
              (r2.tag == TAG_NIL) &&
              (r3.tag == TAG_STR && r3.str == "a") &&
              (r4.tag == TAG_STR && r4.str == "b");

    if (ok) pass("5 pipelined requests -> all correct responses");
    else    fail("5 pipelined requests -> all correct responses");

    cmd(fd, {"del", "p1"});
    cmd(fd, {"del", "p2"});
    cmd(fd, {"del", "p3"});
}

static void test_empty_state(int fd) {
    printf("\n\033[1m[ Empty / boundary conditions ]\033[0m\n");
    Response r;

    // KEYS on empty store
    r = cmd(fd, {"keys"});
    expect_arr_size("keys on empty store -> []", r, 0);

    // SET with empty value
    r = cmd(fd, {"set", "empty_val", ""});
    expect_nil("set empty value -> NIL", r);

    r = cmd(fd, {"get", "empty_val"});
    expect_str("get empty value -> ''", r, "");

    cmd(fd, {"del", "empty_val"});

    // SET with empty key
    r = cmd(fd, {"set", "", "value"});
    expect_nil("set empty key -> NIL", r);

    r = cmd(fd, {"get", ""});
    expect_str("get empty key -> 'value'", r, "value");

    cmd(fd, {"del", ""});
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    printf("\033[1;36m━━━  RedisLite Test Driver  ━━━\033[0m\n");
    printf("Connecting to 127.0.0.1:1234...\n");

    int fd = connect_to_server();
    printf("Connected.\n");

    test_empty_state(fd);
    test_string_ops(fd);
    test_ttl(fd);
    test_sorted_set(fd);
    test_type_errors(fd);
    test_pipelining(fd);

    close(fd);

    int total = g_pass + g_fail;
    printf("\n\033[1m━━━  Results  ━━━\033[0m\n");
    printf("  Total : %d\n", total);
    printf("  \033[32mPassed: %d\033[0m\n", g_pass);
    if (g_fail > 0)
        printf("  \033[31mFailed: %d\033[0m\n", g_fail);
    else
        printf("  \033[32mFailed: 0\033[0m\n");

    if (g_fail == 0)
        printf("\n\033[1;32m✓ All tests passed!\033[0m\n\n");
    else
        printf("\n\033[1;31m✗ %d test(s) failed.\033[0m\n\n", g_fail);

    return g_fail > 0 ? 1 : 0;
}
