#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>   // inet_addr
#include <sys/socket.h>

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    connect(fd, (sockaddr*)&addr, sizeof(addr));   // 拨号连网关

    char msg[100];
    for (int i = 1; i <= 5; ++i) {
        snprintf(msg, sizeof(msg), "heartbeat #%d", i);
        write(fd, msg, strlen(msg));               // 发一帧
        char buf[1024];
        ssize_t n = read(fd, buf, sizeof(buf));    // 收 echo
        printf("[device] got %zd bytes: %.*s\n", n, (int)n, buf);
        sleep(1);                                   // 每秒一次心跳
    }
    close(fd);
    return 0;
}
