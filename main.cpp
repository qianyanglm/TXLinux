//代码清单9-7 回射服务器: 同时处理TCP和UDP服务
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_EVENT_NUMBER 1024
#define TCP_BUFFER_SIZE 512
#define UDP_BUFFER_SIZE 1024

//将文件描述符设置为非阻塞的
int setnonblocking(int fd)
{
    //设置文件描述符
    int old_option = fcntl(fd, F_GETFL);
    //设置非阻塞标志
    int new_option = old_option | O_NONBLOCK;
    //控制文件的各种操作，这里设置fd的标志
    fcntl(fd, F_SETFL, new_option);
    //返回老的状态
    return old_option;
}

//将fd文件描述符上的事件添加到epollfd代表的epoll事件表中
void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    //当该文件描述符上有数据可读时，将触发epoll事件
    event.events = EPOLLIN | EPOLLET;
    //向事件表中注册fd上的事件
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //调用 setnonblocking(fd) 将文件描述符 fd 设置为非阻塞模式，以确保后续的 I/O 操作不会阻塞整个程序
    setnonblocking(fd);
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

    //创建TCP socket 并将其绑定到端口port上
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    ret = bind(listenfd, (struct sockaddr *) &address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    //创建UDP socket, 并将其绑定到端口port上
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);
    int udpfd = socket(PF_INET, SOCK_DGRAM, 0);
    assert(udpfd >= 0);

    ret = bind(udpfd, (struct sockaddr *) &address, sizeof(address));
    assert(ret != -1);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    //注册TCP socket和UDP socket上的可读事件
    addfd(epollfd, listenfd);
    addfd(epollfd, udpfd);

    while (1)
    {
        //当事件发生时，将所有就绪的事件拷贝到events中，并返回就绪的文件描述符个数
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0)
        {
            printf("epoll failure\n");
            break;
        }

        //遍历events数组中就绪的事件
        for (int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *) &client_address, &client_addrlength);
                addfd(epollfd, connfd);
            }
            else if (sockfd == udpfd)
            {
                char buf[UDP_BUFFER_SIZE];
                memset(buf, '\0', UDP_BUFFER_SIZE);
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);

                ret = recvfrom(udpfd, buf, UDP_BUFFER_SIZE - 1, 0, (struct sockaddr *) &client_address, &client_addrlength);
                if (ret > 0)
                {
                    sendto(udpfd, buf, UDP_BUFFER_SIZE - 1, 0, (struct sockaddr *) &client_address, client_addrlength);
                    //自己加的，在终端输出UDP收到的数据
                    printf("receive UDP data: %s\n", buf);
                }
            }
            else if (events[i].events & EPOLLIN)
            {
                char buf[TCP_BUFFER_SIZE];
                while (1)
                {
                    memset(buf, '\0', TCP_BUFFER_SIZE);
                    ret = recv(sockfd, buf, TCP_BUFFER_SIZE - 1, 0);
                    if (ret < 0)
                    {
                        if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
                        {
                            break;
                        }
                        close(sockfd);
                        break;
                    }
                    else if (ret == 0)
                    {
                        close(sockfd);
                    }
                    else
                    {
                        send(sockfd, buf, ret, 0);
                        //自己加的，在终端输出TCP收到的数据
                        printf("receive TCP data: %s\n", buf);
                    }
                }
            }
            else
            {
                printf("something else happened\n");
            }
        }
    }
    close(listenfd);
    return 0;
}