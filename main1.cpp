//代码清单10-1 统一事件源
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
static int pipefd[2];

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

//信号处理函数
