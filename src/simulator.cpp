// 设备压力模拟器：N 台设备并发在线，断线自动重连
// 用法: ./simulator 设备数 运行秒数    例: ./simulator 500 60
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <csignal>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static const uint8_t kMagic = 0xA5;
static std::atomic<bool> g_run{true};
static std::atomic<long> g_online{0};

static std::string makeFrame(uint8_t type, const std::string& payload) {
    std::string f;
    f.push_back((char)kMagic);
    f.push_back((char)(payload.size() >> 8));
    f.push_back((char)(payload.size() & 0xFF));
    f.push_back((char)type);
    f += payload;
    return f;
}

// 一台设备的一生：连接→注册→每秒心跳→断了退避1秒重连
static void deviceLoop(int id) {
    std::string name = "dev-" + std::to_string(1000 + id);
    while (g_run) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9000);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
            close(fd);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        ++g_online;
        std::string reg = makeFrame(0x01, name);
        write(fd, reg.data(), reg.size());

        int seq = 0;
        while (g_run) {
            char s[16];
            snprintf(s, sizeof(s), "%d", ++seq);
            std::string hb = makeFrame(0x02, s);
            if (write(fd, hb.data(), hb.size()) <= 0) break;   // 断线/被踢
            char ack[64];
            if (read(fd, ack, sizeof(ack)) <= 0) break;        // 断线/被踢
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        --g_online;
        close(fd);
        std::this_thread::sleep_for(std::chrono::seconds(1));  // 退避后重连
    }
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);   // 写到被踢连接会 SIGPIPE，忽略后 write 只报错不死人
    int n    = argc > 1 ? atoi(argv[1]) : 100;
    int secs = argc > 2 ? atoi(argv[2]) : 30;

    std::vector<std::thread> pool;
    for (int i = 0; i < n; ++i) pool.emplace_back(deviceLoop, i);

    for (int t = 1; t <= secs; ++t) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        printf("[sim] t=%ds online=%ld\n", t, g_online.load());
    }
    g_run = false;
    for (auto& th : pool) th.join();
    printf("[sim] done\n");
    return 0;
}
