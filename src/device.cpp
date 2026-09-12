#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static const uint8_t kMagic = 0xA5;

static std::string makeFrame(uint8_t type, const std::string& payload) {
    std::string f;
    f.push_back((char)kMagic);
    f.push_back((char)(payload.size() >> 8));    // 长度高位在前 = 大端
    f.push_back((char)(payload.size() & 0xFF));  // 低位在后
    f.push_back((char)type);
    f += payload;
    return f;
}

static void waitAck(int fd) {
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n >= 4 && (uint8_t)buf[0] == kMagic)
        printf("[device] ACK(for 0x%02X)\n", (uint8_t)buf[4]);   // buf[4]=载荷：确认的是哪类消息
}

int main(int argc, char** argv) {
    std::string name = argc > 1 ? argv[1] : "dev-000";
    std::string mode = argc > 2 ? argv[2] : "";

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("connect");
        return 1;
    }

    if (mode == "burst") {
        // 粘包测试：3 帧拼成一串、一次 write 发出
        printf("[device] burst: 3 frames in ONE write\n");
        std::string glued = makeFrame(0x01, name)
                          + makeFrame(0x02, "1")
                          + makeFrame(0x03, "burst-data");
        write(fd, glued.data(), glued.size());
        for (int i = 0; i < 3; ++i) waitAck(fd);

    } else if (mode == "spam") {
        // 慢消费者测试：只发不读，逼网关触发背压
        int total = argc > 3 ? atoi(argv[3]) : 2000000;

        // v5.2 演示旋钮：把我的接收通道也压到 8KB。
        // 原因：TCP 的接收缓冲在内核里，应用不读它也照收——rcvbuf 自动调优到几 MB，
        // 会把网关发来的 ACK 全吞在内核里，背压传导不到应用层（实验实测 ~3MB+）。
        int rcvbuf = 8 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        std::string reg = makeFrame(0x01, name);
        write(fd, reg.data(), reg.size());          // 注册，不等回音
        std::string hb = makeFrame(0x02, "9");
        printf("[device] spam: sending %d frames WITHOUT reading...\n", total);
        for (int i = 0; i < total; ++i) write(fd, hb.data(), hb.size());
        printf("[device] spam sent, draining ACKs...\n");

        long got = 0;
        char buf[4096];
        while (got < 5L * total) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) {
                printf("[device] kicked by gateway after %ld ack bytes\n", got);
                break;
            }
            got += n;
        }

    } else {
        // 正常模式：注册 → 5 次心跳 → 上报 → 退出
        std::string reg = makeFrame(0x01, name);
        write(fd, reg.data(), reg.size());
        waitAck(fd);
        for (int i = 1; i <= 5; ++i) {
            char seq[16];
            snprintf(seq, sizeof(seq), "%d", i);
            std::string hb = makeFrame(0x02, seq);
            write(fd, hb.data(), hb.size());
            waitAck(fd);
            sleep(1);
        }
        std::string rep = makeFrame(0x03, "temp=36.5 volt=220");
        write(fd, rep.data(), rep.size());
        waitAck(fd);
    }
    close(fd);
    return 0;
}
