#include <cstdio>
#include <unistd.h>      // read/write/close
#include <netinet/in.h>  // sockaddr_in 结构体
#include <arpa/inet.h>   // htons
#include <sys/socket.h>  // socket/bind/listen/accept

int main() {
    // ① socket：装一部电话机（AF_INET=IPv4，SOCK_STREAM=TCP）
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);

    // 端口复用：程序重启不用等上次的 TIME_WAIT（26块复习时回来看为什么）
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // ② bind：把电话机登记到 9000 端口
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  // 0.0.0.0：任意网卡来的连接都接
    addr.sin_port = htons(9000);        // 端口号转网络字节序（大端）
    bind(listenFd, (sockaddr*)&addr, sizeof(addr));

    // ③ listen：转为监听状态，开始等人敲门
    listen(listenFd, 1024);
    printf("[gateway] listening on 9000\n");

    // ④ accept：接一次电话，返回一条专属线路 connFd
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    int connFd = accept(listenFd, (sockaddr*)&peer, &len);
    printf("[gateway] one device connected\n");

    // ⑤ 循环收发：收到什么原样写回 = echo
    char buf[4096];
    while (true) {
        ssize_t n = read(connFd, buf, sizeof(buf));
        if (n <= 0) { printf("[gateway] device closed\n"); break; }
        write(connFd, buf, n);
        printf("[gateway] echoed %zd bytes\n", n);
    }
    close(connFd);
    close(listenFd);
    return 0;
}
