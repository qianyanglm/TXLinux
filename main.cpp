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

struct client_data
{
    sockaddr_in address;  //客户端socket地址
    char *write_buf;      //待写到客户端的数据的位置，
    char buf[BUFFER_SIZE];//存储从客户端接收的数据
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

    //创建users数组，分配FD_LIMIT个client_data对象，每个元素对应一个客户端连接
    //存储客户端相关的数据，包括客户端地址，读写缓冲区等
    client_data *users = new client_data[FD_LIMIT];

    pollfd fds[USER_LIMIT + 1];
    int user_counter = 0;
    for (int i = 0; i <= USER_LIMIT; ++i)
    {
        fds[i].fd = -1;//初始化为无效的文件描述符
        fds[i].events = 0;
    }
    fds[0].fd = listenfd;            //存储文件描述符
    fds[0].events = POLLIN | POLLERR;//存储需要监听的可读和错误事件

    //存储实际发生事件。初始为0代表未发生
    fds[0].revents = 0;

    while (1)
    {
        ret = poll(fds, user_counter + 1, -1);
        if (ret < 0)
        {
            printf("poll failure\n");
        }

        //每次poll返回时，遍历所有文件描述符
        for (int i = 0; i < user_counter + 1; ++i)
        {
            //如果监听套接字上有POLLIN事件，表示有新的客户端连接请求
            if ((fds[i].fd == listenfd) && (fds[i].revents & POLLIN))
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                //接受新的客户端连接，创建新连接的套接字描述符
                int connfd = accept(listenfd, (struct sockaddr *) &client_address, &client_addrlength);

                //连接出错
                if (connfd < 0)
                {
                    printf("errno is :%d\n", errno);
                    continue;
                }
                //如果请求太多，则关闭新到的连接
                //所以最多只能有USER_LIMIT个用户
                if (user_counter >= USER_LIMIT)
                {
                    const char *info = "too many users\n";
                    printf("%s", info);
                    send(connfd, info, strlen(info), 0);
                    close(connfd);
                    continue;
                }

                //新的连接同时修改fds和users数组
                user_counter++;//更新用户计数器
                //将新连接信息加入users数组
                users[connfd].address = client_address;
                setnonblocking(connfd);
                fds[user_counter].fd = connfd;
                //设置可读、关闭和错误事件
                fds[user_counter].events = POLLIN | POLLRDHUP | POLLERR;
                //表示是全新事件
                fds[user_counter].revents = 0;
                printf("comes a new user, now have %d users\n", user_counter);
            }//如果监听套接字有错误
            else if (fds[i].revents & POLLERR)
            {
                printf("get an error from %d\n", fds[i].fd);
                char errors[100];
                memset(errors, '\0', 100);
                socklen_t length = sizeof(errors);
                if (getsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, &errors, &length) < 0)
                {
                    printf("get socket option failed\n");
                }
                continue;
            }//TCP连接被对方关闭，或者对方关闭了写操作
            else if (fds[i].revents & POLLRDHUP)
            {
                //如果客户端关闭连接，则服务器也关闭对应的连接，并将用户总数-1
                users[fds[i].fd] = users[fds[user_counter].fd];
                close(fds[i].fd);
                fds[i] = fds[user_counter];
                i--;
                user_counter--;
                printf("a client left\n");
            }//如果监听套接字可以读取
            else if (fds[i].revents & POLLIN)
            {
                int connfd = fds[i].fd;
                memset(users[connfd].buf, '\0', BUFFER_SIZE);
                ret = recv(connfd, users[connfd].buf, BUFFER_SIZE - 1, 0);
                printf("get %d bytes of client data %s from %d\n", ret, users[connfd].buf, connfd);
                if (ret < 0)
                {
                    //如果读操作出错则关闭连接
                    if (errno != EAGAIN)
                    {
                        close(connfd);
                        users[fds[i].fd] = users[fds[user_counter].fd];
                        fds[i] = fds[user_counter];
                        i--;
                        user_counter--;
                    }
                }
                else if (ret == 0)
                {
                }
                else
                {
                    //如果接收到客户数据，则通知其他socket连接准备写数据
                    for (int j = 1; j <= user_counter; ++j)
                    {
                        if (fds[j].fd == connfd)
                        {
                            continue;
                        }
                        fds[j].events |= ~POLLIN;
                        fds[j].events |= ~POLLOUT;
                        users[fds[j].fd].write_buf = users[connfd].buf;
                    }
                }
            }//监听套接字可写
            else if (fds[i].revents & POLLOUT)
            {
                int connfd = fds[i].fd;
                if (!users[connfd].write_buf)
                {
                    continue;
                }
                send(connfd, users[connfd].write_buf, strlen(users[connfd].write_buf), 0);
                users[connfd].write_buf = nullptr;
                //写完数据后需要重新注册fds[i]上的可读事件
                //不再准备可写事件
                fds[i].events |= ~POLLOUT;
                //注册可读事件
                fds[i].events |= POLLIN;
            }
        }
    }

    delete[] users;
    close(listenfd);

    return 0;
}