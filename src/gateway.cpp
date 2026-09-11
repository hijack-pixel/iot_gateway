#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

// ---------- 协议：[0xA5][len 2B大端][type 1B][payload len B] ----------
static const uint8_t  kMagic = 0xA5;
static const uint16_t kMaxFrame = 4096;   // 异常长度防御：超过就当脏数据扔

enum MsgType : uint8_t {
    MSG_REGISTER  = 0x01,   // 设备→网关：payload=设备名
    MSG_HEARTBEAT = 0x02,   // 设备→网关：payload=序号
    MSG_REPORT    = 0x03,   // 设备→网关：payload=数据
    MSG_ACK       = 0x81,   // 网关→设备：payload=被确认的消息类型
};

// 每条连接的会话状态 —— Day 4 注册表/心跳的雏形就从这长出来
struct Session {
    std::string rbuf;       // 接收缓冲：粘包/半包的主战场
    std::string name;       // 注册时上报的设备名（没注册=空）
    int frames = 0;         // 该连接累计收到几帧
};

// 网关回 ACK：亲手组一个帧，体验"按协议打包"
static void sendAck(int fd, uint8_t ofType) {
    char f[5] = { (char)kMagic, 0, 1, (char)MSG_ACK, (char)ofType };
    //         魔数      len高 len低(=1)  类型        载荷
    write(fd, f, 5);
}

// ---------- 解包状态机：从 rbuf 里尽力切帧，切不动就留着等 ----------
static void processInput(int fd, Session& s) {
    while (true) {
        size_t have = s.rbuf.size();
        if (have < 4) break;                        // 帧头都没凑齐 → 半包，留着

        if ((uint8_t)s.rbuf[0] != kMagic) {         // 魔数不对 → 脏数据
            s.rbuf.erase(0, 1);                     // 扔 1 字节继续找帧头（重同步）
            continue;
        }
        uint16_t len = ((uint8_t)s.rbuf[1] << 8) | (uint8_t)s.rbuf[2];  // 大端拼长度
        if (len > kMaxFrame) {                      // 声称超长 → 当脏数据扔
            s.rbuf.erase(0, 1);
            continue;
        }
        if (have < 4 + (size_t)len) break;          // 帧身没到齐 → 半包，留着

        uint8_t type = (uint8_t)s.rbuf[3];
        std::string payload = s.rbuf.substr(4, len);
        s.rbuf.erase(0, 4 + (size_t)len);           // 消费这一帧；粘包的话下一帧继续切

        ++s.frames;
        if (type == MSG_REGISTER) {
            s.name = payload;
            printf("[gateway] fd#%d registered as '%s'\n", fd, s.name.c_str());
        } else if (type == MSG_HEARTBEAT) {
            printf("[gateway] '%s' heartbeat seq=%s\n", s.name.c_str(), payload.c_str());
        } else if (type == MSG_REPORT) {
            printf("[gateway] '%s' report: %s\n", s.name.c_str(), payload.c_str());
        }
        sendAck(fd, type);
    }
}

int main() {
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9000);
    bind(listenFd, (sockaddr*)&addr, sizeof(addr));
    listen(listenFd, 1024);
    printf("[gateway] listening on 9000\n");

    int epFd = epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listenFd;
    epoll_ctl(epFd, EPOLL_CTL_ADD, listenFd, &ev);

    std::unordered_map<int, Session> sessions;      // fd → 会话

    epoll_event events[64];
    while (true) {
        int n = epoll_wait(epFd, events, 64, -1);
        if (n < 0) { perror("epoll_wait"); break; }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listenFd) {
                sockaddr_in peer{};
                socklen_t len = sizeof(peer);
                int connFd = accept(listenFd, (sockaddr*)&peer, &len);
                printf("[gateway] device fd#%d connected\n", connFd);

                epoll_event cev{};
                cev.events = EPOLLIN;
                cev.data.fd = connFd;
                epoll_ctl(epFd, EPOLL_CTL_ADD, connFd, &cev);
                sessions[connFd] = Session{};       // 新连接 = 新会话
            } else {
                char buf[4096];
                ssize_t cnt = read(fd, buf, sizeof(buf));
                if (cnt <= 0) {
                    Session& s = sessions[fd];
                    printf("[gateway] '%s'(fd#%d) closed, %d frames\n",
                           s.name.c_str(), fd, s.frames);
                    sessions.erase(fd);
                    epoll_ctl(epFd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                } else {
                    sessions[fd].rbuf.append(buf, cnt);  // 新字节追加进缓冲
                    processInput(fd, sessions[fd]);      // 状态机尽力切帧
                }
            }
        }
    }
    close(listenFd);
    return 0;
}
