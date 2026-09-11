#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static const uint8_t kMagic = 0xA5;

// 组帧：按协议打包（设备侧的"打包器"，对应网关的解包器）
static std::string makeFrame(uint8_t type, const std::string& payload) {
    std::string f;
    f.push_back((char)kMagic);
    f.push_back((char)(payload.size() >> 8));    // 长度高位在前 = 大端 = 网络字节序
    f.push_back((char)(payload.size() & 0xFF));  // 低位在后
    f.push_back((char)type);
    f += payload;
    return f;
}

static void waitAck(int fd) {
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n >= 4 && (uint8_t)buf[0] == kMagic)
        printf("[device] ACK(for 0x%02X)\n", (uint8_t)buf[3]);
}

int main(int argc, char** argv) {
    // 用法: ./device 设备名 [burst]   ← argc/argv：命令行参数，main 的老朋友
    std::string name = argc > 1 ? argv[1] : "dev-000";
    bool burst = (argc > 2 && std::string(argv[2]) == "burst");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("connect");
        return 1;
    }

    if (!burst) {
        // 正常模式：注册 → 心跳×5 → 上报，每帧等 ACK
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
    } else {
        // 粘包测试模式：把 3 帧 拼进一个大字符串，一次 write 发出去
        printf("[device] burst: 3 frames in ONE write\n");
        std::string glued = makeFrame(0x01, name)
                          + makeFrame(0x02, "1")
                          + makeFrame(0x03, "burst-data");
        write(fd, glued.data(), glued.size());
        for (int i = 0; i < 3; ++i) waitAck(fd);   // 应收到 3 个 ACK
    }
    close(fd);
    return 0;
}
