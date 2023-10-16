//代码清单9-7 服务器
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define USER_LIMIT 5  //最大用户数量
#define BUFFER_SIZE 64//读缓冲区的大小
#define FD_LIMIT 65535//文件描述符数量限制

//客户数据
//客户端socket地址，待写到客户端的数据的位置，从客户端读入的数据
struct client_data
{
    sockaddr_in address;
    char *write_buf;
    char buf[BUFFER_SIZE];
};

//将文件描述符设置为非阻塞的
int setnonblocking(int fd)
{
    //获取文件描述符旧的状态标志
    int old_option = fcntl(fd, F_GETFL);
    //设置非阻塞标志
    int new_option = old_option | O_NONBLOCK;
    //控制文件的各种操作，这里是设置fd的状态标志
    fcntl(fd, F_SETFL, new_option);
    return old_option;//返回文件描述符旧的状态
}

int main(int argc, char *argv[])
{
    if (argc <= 2)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    return 0;
}