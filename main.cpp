#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "lst_timer.h"

#define FD_LIMIT 65535
#define MAX_EVENT_NUMBER 1024
#define TIMESLOT 5

static int pipefd[2];
//利用代码清单11-2中的升序链表来管理定时器
static sort_timer_lst timer_lst;
static int epollfd = 0;

//将文件描述符设置为非阻塞的
int setnonblocking(int fd)
{
    //获取文件描述符的状态
    int old_option = fcntl(fd, F_GETFL);
    //设置非阻塞标志
    int new_option = old_option | O_NONBLOCK;
    //控制文件的各种操作，这里设置fd的标志
    fcntl(fd, F_SETFL, new_option);
    //返回文件原来的的状态
    return old_option;
}

//将fd文件描述符上的事件添加到epollfd代表的epoll事件表中
void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    //当该文件描述符上有数据可读时，将处罚epoll事件
    event.events = EPOLLIN | EPOLLET;
    //向该事件表中注册fd上的事件
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //将fd设置为非阻塞模式
    setnonblocking(fd);
}

//信号处理函数
void sig_handler(int sig)
{
    //保留原来的errno，在函数最后恢复，以保证函数的可重入性
    int save_errno = errno;
    int msg = sig;
    //将信号写入管道，以通知主循环
    send(pipefd[1], (char *) &msg, 1, 0);
    errno = save_errno;
}

//信号处理函数
void addsig(int sig)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void timer_handler()
{
    //定时处理任务，实际上就是调用tick函数
    timer_lst.tick();
    //调用alarm一次只会引起一次SIGALRM信号，所以要重新定时不断触发SIGALRM信号
    alarm(TIMESLOT);
}

//定时器回调函数，删除非活动连接socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    printf("close fd %d\n", user_data->sockfd);
}

int main(int argc, char *argv[])
{
    if (argc <= 2)
    {
        printf("usgae : %s ip_address port_number\n", basename(argv[0]));
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

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd, listenfd);

    //使用socketpair创建管道，注册pipefd[0]上的可读事件
    //socketpair创建的管道既可读又可写
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0]);

    //设置信号处理函数
    addsig(SIGALRM);//定时器信号，触发定时任务
    addsig(SIGTERM);//终止信号，优雅的关闭服务器
    bool stop_server = false;

    //存储客户端连接信息
    client_data *users = new client_data[FD_LIMIT];
    bool timeout = false;
    //启动定时器，定时触发SIGALRM信号，以执行定时任务
    alarm(TIMESLOT);//定时

    while (!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            //处理新到的客户连接
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *) &client_address, &client_addrlength);
                addfd(epollfd, connfd);
                users[connfd].address = client_address;
                users[connfd].sockfd = connfd;
                //创建定时器，设置其回调函数遇超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer *timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].timer = timer;
                timer_lst.add_timer(timer);
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))//处理信号
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    //handle the error
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                                //定时器信号，触发定时任务
                            case SIGALRM:
                            {
                                //用timeout变量标记有定时任务需要处理,但不立即处理定时任务，这是因为定时任务的优先级不是很高，优先处理其他更重要的任务
                                timeout = true;
                                break;
                            }
                                //终止服务器信号
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            //处理客户连接上收到的数据
            else if (events[i].events & EPOLLIN)
            {
                memset(users[sockfd].buf, '\0', BUFFER_SIZE);
                ret = recv(sockfd, users[sockfd].buf, BUFFER_SIZE - 1, 0);
                printf("get %d bytes of client data %s from %d\n", ret, users[sockfd].buf, sockfd);

                util_timer *timer = users[sockfd].timer;
                if (ret < 0)
                {
                    //如果发生读错误，关闭连接，并移除其对应的定时器
                    if (errno != EAGAIN)
                    {
                        cb_func(&users[sockfd]);
                        if (timer)
                        {
                            timer_lst.del_timer(timer);
                        }
                    }
                }
                else if (ret == 0)
                {
                    //如果对方已经关闭连接，则我们也关闭连接，并移除对应的定时器
                    cb_func(&users[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
                else
                {
                    //如果某个客户连接上有数据可读，则我们要调整该连接对应的定时器，以延迟该链接被关闭的时间
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        printf("adjust timer once\n");
                        timer_lst.adjust_timer(timer);
                    }
                }
            }
            else
            {
                //others
            }
            if (timeout)
            {
                timer_handler();
                timeout = false;
            }
        }
    }
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    return 0;
}