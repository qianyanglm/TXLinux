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

    ret = bind(listenfd, (struct sockaddr *) &address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    //创建users数组，分配FD_LIMIT个client_data对象，可以预期
    client_data *users = new client_data[FD_LIMIT];
    pollfd fds[USER_LIMIT + 1];
    int user_counter = 0;
    for (int i = 0; i <= USER_LIMIT; ++i)
    {
        fds[i].fd = -1;
        fds[i].events = 0;
    }
    fds[0].fd = listenfd;
    fds[0].events = POLLIN | POLLERR;
    fds[0].revents = 0;

    while (1)
    {
        ret = poll(fds, user_counter + 1, -1);
        if (ret < 0)
        {
            printf("poll failure\n");
        }

        for (int i = 0; i < user_counter + 1; ++i)
        {
            if ((fds[i].fd == listenfd) && (fds[i].revents & POLLIN))
            {
            }
        }
    }


    return 0;
}