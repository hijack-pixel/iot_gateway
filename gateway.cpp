#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <string>
#include <unordered_map>
#include <chrono>
#include <thread>             // ★v7：工人线程
#include <vector>
#include <mutex>              // ★v7：锁
#include <condition_variable> // ★v7：条件变量（工人等活）
#include <queue>              // ★v7：任务/结果队列
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>      // ★v7：跨线程门铃

static const uint8_t  kMagic = 0xA5;
static const uint16_t kMaxFrame = 4096;
static const int      kKickMs  = 5000;
static const size_t   kMaxSbuf = 1 << 20;
static const int      kWorkIter = 2000000;   // ★v7：每帧业务量（200万次哈希≈2ms，可调）

enum MsgType : uint8_t {
    MSG_REGISTER = 0x01, MSG_HEARTBEAT = 0x02, MSG_REPORT = 0x03, MSG_ACK = 0x81,
};

struct Session {
    std::string rbuf;
    std::string sbuf;
    std::string name;
    int  frames = 0;
    bool overflow = false;
    std::chrono::steady_clock::time_point lastRecv = std::chrono::steady_clock::now();
};

// ★v7：跨线程搬运的两种包裹——都是"拷贝"，工人不碰 Session
struct Task   { int fd; uint8_t type; std::string payload; };  // 主→工人：一帧
struct Result { int fd; uint8_t ackType; };                    // 工人→主：要回的ACK

static bool g_quiet = false;

// ★v7：流水线的全局设施
static std::queue<Task>   g_tasks;                 // 任务队列（传送带）
static std::queue<Result> g_results;               // 结果队列
static std::mutex              g_mtx;              // 护任务队列
static std::mutex              g_outMtx;           // 护结果队列
static std::condition_variable g_cv;               // 工人等活的铃
static int g_evFd = -1;                            // eventfd：工人按给主线程的门铃

static void setNonBlock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 发送：尽量发，发不完入队并挂 EPOLLOUT（同 v6）
static void appendOut(int epFd, int fd, Session& s, const char* data, size_t len) {
    if (s.sbuf.empty()) {
        ssize_t n = write(fd, data, len);
        if (n < 0) n = 0;
        if ((size_t)n == len) return;
        data += n; len -= (size_t)n;
    }
    s.sbuf.append(data, len);
    if (s.sbuf.size() > kMaxSbuf) {
        s.overflow = true;
        return;
    }
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = fd;
    epoll_ctl(epFd, EPOLL_CTL_MOD, fd, &ev);
}

static void onWritable(int epFd, int fd, Session& s) {
    while (!s.sbuf.empty()) {
        ssize_t n = write(fd, s.sbuf.data(), s.sbuf.size());
        if (n > 0) { s.sbuf.erase(0, (size_t)n); continue; }
        break;
    }
    if (s.sbuf.empty()) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        epoll_ctl(epFd, EPOLL_CTL_MOD, fd, &ev);
    }
}

// ★v7：主线程的切帧器——从 processInput 拆出来的"前半段"
// 只切帧、装 Task 上传送带；业务处理交给工人
static int extractTasks(int fd, Session& s) {
    int cut = 0;
    while (true) {
        size_t have = s.rbuf.size();
        if (have < 4) break;
        if ((uint8_t)s.rbuf[0] != kMagic) { s.rbuf.erase(0, 1); continue; }
        uint16_t len = ((uint8_t)s.rbuf[1] << 8) | (uint8_t)s.rbuf[2];
        if (len > kMaxFrame) { s.rbuf.erase(0, 1); continue; }
        if (have < 4 + (size_t)len) break;

        uint8_t type = (uint8_t)s.rbuf[3];
        std::string payload = s.rbuf.substr(4, len);
        s.rbuf.erase(0, 4 + (size_t)len);

        ++cut; ++s.frames;
        {
            std::lock_guard<std::mutex> lk(g_mtx);       // 上传送带（锁一下）
            g_tasks.push(Task{ fd, type, std::move(payload) });
        }
        g_cv.notify_one();                                // 摇醒一个工人
    }
    return cut;
}

// ★v7：工人的工作日——等活、干活、交活、按门铃
static void workerLoop() {
    while (true) {
        Task t;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return !g_tasks.empty(); });   // 没活就睡（第20块知识！）
            t = g_tasks.front();
            g_tasks.pop();
        }
        // ===== 重业务：对 payload 做 200 万轮哈希（模拟查库/加密等真实开销）=====
        uint32_t h = 2166136261u;
        for (int i = 0; i < kWorkIter; ++i) {
            h ^= (uint32_t)payload_len(t) + (uint32_t)i;
            h *= 16777619u;
        }
        (void)h;
        // ===== 干完交活 =====
        {
            std::lock_guard<std::mutex> lk(g_outMtx);
            g_results.push(Result{ t.fd, t.type });
        }
        uint64_t one = 1;
        ::write(g_evFd, &one, sizeof(one));   // 按门铃：叫主线程来收货
    }
}

static void closeConn(int epFd, std::unordered_map<int, Session>& sessions, int fd, const char* why) {
    Session& s = sessions[fd];
    printf("[gateway] '%s'(fd#%d) %s, %d frames\n", s.name.c_str(), fd, why, s.frames);
    sessions.erase(fd);
    epoll_ctl(epFd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}

int main(int argc, char** argv) {
    g_quiet = (argc > 1 && std::string(argv[1]) == "--quiet");
    int nWorkers = argc > 2 ? atoi(argv[2]) : 4;      // ★v7：工人数量，默认4
    signal(SIGPIPE, SIG_IGN);

    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9000);
    bind(listenFd, (sockaddr*)&addr, sizeof(addr));
    listen(listenFd, 1024);
    setNonBlock(listenFd);
    printf("[gateway] listening on 9000 (ET + %d workers)\n", nWorkers);

    int epFd = epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listenFd;
    epoll_ctl(epFd, EPOLL_CTL_ADD, listenFd, &ev);

    // ★v7：门铃登记进花名册——从此工人的呼唤和设备的消息走同一套 epoll
    g_evFd = eventfd(0, 0);
    epoll_event eev{};
    eev.events = EPOLLIN;                   // eventfd 用 LT 即可（读一次清一次）
    eev.data.fd = g_evFd;
    epoll_ctl(epFd, EPOLL_CTL_ADD, g_evFd, &eev);

    // ★v7：开工——拉起工人队伍
    std::vector<std::thread> pool;
    for (int i = 0; i < nWorkers; ++i)
        pool.emplace_back(workerLoop);

    std::unordered_map<int, Session> sessions;
    long msgs = 0, wakes = 0;

    epoll_event events[64];
    while (true) {
        int n = epoll_wait(epFd, events, 64, 1000);
        if (n < 0) { perror("epoll_wait"); break; }
        if (n > 0) ++wakes;

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listenFd) {
                while (true) {
                    sockaddr_in peer{};
                    socklen_t len = sizeof(peer);
                    int connFd = accept(listenFd, (sockaddr*)&peer, &len);
                    if (connFd < 0) break;
                    if (!g_quiet) printf("[gateway] device fd#%d connected\n", connFd);
                    setNonBlock(connFd);
                    int sndbuf = 8 * 1024;
                    setsockopt(connFd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLET;
                    cev.data.fd = connFd;
                    epoll_ctl(epFd, EPOLL_CTL_ADD, connFd, &cev);
                    sessions[connFd] = Session{};
                }

            } else if (fd == g_evFd) {
                // ★v7：门铃响了——收走结果队列，统一回 ACK（IO 仍只在主线程）
                uint64_t cnt = 0;
                read(g_evFd, &cnt, sizeof(cnt));
                while (true) {
                    Result r;
                    {
                        std::lock_guard<std::mutex> lk(g_outMtx);
                        if (g_results.empty()) break;
                        r = g_results.front();
                        g_results.pop();
                    }
                    if (!sessions.count(r.fd)) continue;   // 干活期间连接已死：丢弃
                    char ack[5] = { (char)kMagic, 0, 1, (char)MSG_ACK, (char)r.ackType };
                    appendOut(epFd, r.fd, sessions[r.fd], ack, 5);
                }

            } else {
                uint32_t ee = events[i].events;
                bool dead = false;

                if (ee & EPOLLOUT)
                    onWritable(epFd, fd, sessions[fd]);

                if (ee & EPOLLIN) {
                    char buf[4096];
                    bool got = false;
                    while (true) {
                        ssize_t cnt = read(fd, buf, sizeof(buf));
                        if (cnt > 0) { sessions[fd].rbuf.append(buf, cnt); got = true; }
                        else if (cnt == 0) { dead = true; break; }
                        else { if (errno != EAGAIN) dead = true; break; }
                    }
                    if (got && !dead) {
                        msgs += extractTasks(fd, sessions[fd]);   // ★改：切帧上传送带
                        sessions[fd].lastRecv = std::chrono::steady_clock::now();
                    }
                }

                if (sessions.count(fd) && sessions[fd].overflow) dead = true;

                if (dead) {
                    if (!sessions.count(fd)) continue;
                    if (sessions[fd].overflow)
                        closeConn(epFd, sessions, fd, "KICK: send-queue overflow");
                    else
                        closeConn(epFd, sessions, fd, "closed");
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        for (auto it = sessions.begin(); it != sessions.end(); ) {
            long silentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - it->second.lastRecv).count();
            if (silentMs > kKickMs) {
                printf("[gateway] KICK '%s'(fd#%d): silent %ldms\n",
                       it->second.name.c_str(), it->first, silentMs);
                epoll_ctl(epFd, EPOLL_CTL_DEL, it->first, nullptr);
                close(it->first);
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
        // ★v7：仪表加了两个队列水位 q/r（q>0 = 业务跟不上，传送带积压）
        size_t qn, rn;
        { std::lock_guard<std::mutex> a(g_mtx); qn = g_tasks.size(); }
        { std::lock_guard<std::mutex> b(g_outMtx); rn = g_results.size(); }
        printf("[stat] online=%zu msgs=%ld wakes=%ld q=%zu r=%zu\n",
               sessions.size(), msgs, wakes, qn, rn);
    }
    close(listenFd);
    return 0;
}
