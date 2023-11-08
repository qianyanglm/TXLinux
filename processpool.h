//
// Created by A qian yang on 2023/11/6.
//
//代码清单15-1
#ifndef PROCESSPOOL_H_
#define PROCESSPOOL_H_
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

//子进程的类
class process
{
public:
    process(): m_pid(-1) {}

    //写两个public为了代码的可读性
public:
    pid_t m_pid;    //目标子进程的PID
    int m_pipefd[2];//父进程和子进程通信用的管道
};

//进程池类，将它定义为模板类是为了代码复用
//模板参数是处理逻辑任务的类
template<typename T>
class processpool
{
private:
    //将构造函数定义为私有的，因此我们只能通过后面的create静态函数来创建processpool实例
    processpool(int listenfd, int process_number = 8);

public:
    //单体模式，以保证程序最多创建一个processpool实例，这是程序正确处理信号的必要条件
    static processpool<T> *create(int listenfd, int process_number = 8)
    {
        if (!m_instance)
        {
            m_instance = new processpool<T>(listenfd, process_number);
        }
        return m_instance;
    }

    ~processpool()
    {
        delete[] m_sub_process;
    }

    //启动进程池
    void run();

private:
    //统一事件源
    void setup_sig_pipe();
    void run_parent();
    void run_child();

private:
    //进程池允许的最大子进程数量
    static const int MAX_PROCESS_NUMBER = 16;
    //每个子进程最多能处理的客户数量
    static const int USER_PER_PROCESS = 65536;
    //epoll最多能处理的事件数
    static const int MAX_EVENT_NUMBER = 10000;
    //进程池中的进程总数
    int m_process_number;
    //子进程在池中的序号，从0开始
    int m_idx;
    //每个进程都有一个epoll内核事件表，用m_epollfd标识
    int m_epollfd;
    //监听socket
    int m_listenfd;
    //子进程通过m_stop来决定是否停止运行
    int m_stop;
    //保存所有子进程的描述信息
    process *m_sub_process;
    //进程池静态实例
    static processpool<T> *m_instance;
};

//进程池静态实例
template<typename T>
processpool<T> *processpool<T>::m_instance = NULL;

//用于处理信号的管道，以实现统一事件源，后面称之为信号管道
static int sig_pipefd[2];

//设置文件描述符为非阻塞的
static int setnonblocking(int fd)
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
    //当该文件描述符上有数据可读时，将处罚epoll事件
    event.events = EPOLLIN | EPOLLET;
    //向该事件表中注册fd上的事件
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //设置文件描述符为非阻塞
    setnonblocking(fd);
}

//从epollfd标识的epoll内核事件表中删除fd上的所有注册事件
static void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//信号处理函数
void sig_handler(int sig)
{
    //保留原来的errno，在函数最后恢复，以保证函数的可重入性
    int save_errno = errno;
    int msg = sig;
    //将信号写入管道，以通知主循环
    send(sig_pipefd[1], (char *) &msg, 1, 0);
    errno = save_errno;
}

//注册信号处理函数
void addsig(int sig, void(handler)(int), bool restart = true)
{
    //信号处理相关设置的结构体
    struct sigaction sa;
    //清零初始化sa,避免随机垃圾值
    memset(&sa, '\0', sizeof(sa));
    //设置sig触发时调用的函数
    sa.sa_handler = handler;
    if (restart)
    {
        //设置标志位，被信号中断的系统调用可以重启
        sa.sa_flags |= SA_RESTART;
    }
    //信号执行期间阻塞其他信号，防止递归调用
    sigfillset(&sa.sa_mask);
    //注册信号处理函数
    assert(sigaction(sig, &sa, NULL) != -1);
}

//进程池构造函数。 参数listenfd是监听socket，必须在创建进程池前创建，否则子进程无法直接引用它
//process_number指定进程池中子进程的数量
template<typename T>
processpool<T>::processpool(int listenfd, int process_number): m_listenfd(listenfd), m_process_number(process_number), m_idx(-1), m_stop(false)
{
    assert((process_number > 0) && (process_number <= MAX_PROCESS_NUMBER));

    //创建process_number个子进程
    m_sub_process = new process[process_number];
    assert(m_sub_process);

    //创建process_number个子进程，并建立他们和父进程之间的管道
    for (int i = 0; i < process_number; ++i)
    {
        int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd);
        //socketpair成功返回0
        assert(ret == 0);

        m_sub_process[i].m_pid = fork();
        assert(m_sub_process[i].m_pid >= 0);
        //父进程关闭写端，父进程不需要写入套接字
        if (m_sub_process[i].m_pid > 0)
        {
            close(m_sub_process[i].m_pipefd[1]);
        }
        //子进程
        else
        {
            //关闭读端，子进程不需要读取套接字
            close(m_sub_process[i].m_pipefd[0]);
            //子进程序号
            m_idx = i;
            //跳出循环，因为子进程不需要继续创建其他子进程
            break;
        }
    }
}

//统一事件源
template<typename T>
void processpool<T>::setup_sig_pipe()
{
    //创建epoll事件监听表和信号管道
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
    assert(ret != -1);

    setnonblocking(sig_pipefd[1]);
    addfd(m_epollfd, sig_pipefd[0]);

    //为以下四个设置处理函数
    //子进程状态改变时告诉父进程
    addsig(SIGCHLD, sig_handler);
    //终止进程的终止信号
    addsig(SIGTERM, sig_handler);
    //由用户输入发送给进程的中断信号
    addsig(SIGINT, sig_handler);
    //当进程试图向一个已经关闭的管道写入数据时，发送给该进程的信号
    addsig(SIGPIPE, sig_handler);
}

//父进程中m_idx值为-1，子进程m_idx>=0，以此判断运行的是父/子进程代码
template<typename T>
void processpool<T>::run()
{
    if (m_idx != -1)
    {
        run_child();
        return;
    }
    run_parent();
}

//子进程运行函数
template<typename T>
void processpool<T>::run_child()
{
    setup_sig_pipe();

    //每个子进程都通过其在进程池中的序号值m_idx找到与父进程通信的管道
    int pipefd = m_sub_process[m_idx].m_pipefd[1];
    //子进程需要监听管道文件描述符pipefd，因为父进程将通过它来通知子进程accept新连接
    addfd(m_epollfd, pipefd);

    //初始化epoll事件数组和用户数组
    epoll_event events[MAX_EVENT_NUMBER];
    T *users = new T[USER_PER_PROCESS];
    assert(users);
    //用户数量初始化为0
    int number = 0;
    int ret = -1;

    //不断等待事件发生
    while (!m_stop)
    {
        //number存储发生的事件数
        number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        //epoll_wait返回-1且不是EINTR(信号中断)错误
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        //在循环中遍历所有发生的事件，对每个事件进行处理
        for (int i = 0; i < number; ++i)
        {
            //获取事件发生的文件描述符
            int sockfd = events[i].data.fd;
            //如果是父进程的管道文件描述符，即新连接到啦来
            if ((sockfd == pipefd) && (events[i].events & EPOLLIN))
            {
                int client = 0;
                //从父子进程之间的管道读取数据，并将结果保存在变量client中，如果读取成功则表示有新客户连接要到来
                ret == recv(sockfd, (char *) &client, sizeof(client), 0);
                if (((ret < 0) && (errno != EAGAIN)) || ret == 0)
                {
                    continue;
                }
                else
                {
                    struct sockaddr_in client_address;
                    socklen_t client_addrlength = sizeof(client_address);
                    int connfd = accept(m_listenfd, (struct sockaddr *) &client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        printf("errno is: %d\n", errno);
                        continue;
                    }
                    addfd(m_epollfd, connfd);
                    //模板类T必须实现init方法，以初始化一个客户连接，我们直接使用connfd来索引逻辑处理对象(T类型对象)，以提高程序效率
                    users[connfd].init(m_epollfd, connfd, client_address);
                }
            }
            //是否是来自父进程的信号管道文件描述符。如果是，则表示有信号到来。此时，根据信号类型进行处理。
            //下面处理子进程接受到的信号
            else if ((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                                //子进程退出
                            case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                while ((pid = waitpid(-1, &stat, WNOHANG)) > 0)
                                {
                                    continue;
                                }
                                break;
                            }
                                //进程需要退出，退出循环
                            //终止信号
                            case SIGTERM:
                                //中断信号
                            case SIGINT:
                            {
                                m_stop = true;
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            //如果是其他可读数据或者文件描述符，必然是客户请求到来，调用逻辑处理对象的process方法处理
            else if (events[i].events & EPOLLIN)
            {
                users[sockfd].process();
            }
            //其他事件就忽略
            else
            {
                continue;
            }
        }
    }

    delete[] users;
    users = NULL;
    close(pipefd);
    //创建m_listenfd的人应该销毁这个
    //    close(m_listenfd);
    close(m_epollfd);
}

//父进程运行函数
template<typename T>
void processpool<T>::run_parent()
{
    //创建信号管道，用于父进程和子进程之间的通信
    setup_sig_pipe();

    //父进程监听m_listenfd
    //将请求文件描述符和信号管道文件描述符添加到epoll实例中
    addfd(m_epollfd, m_listenfd);

    //epoll事件数组，用于存储epoll监听到的事件
    epoll_event events[MAX_EVENT_NUMBER];
    //记录下一个要分配新连接的子进程的索引
    int sub_process_counter = 0;
    //新连接请求
    int new_conn = 1;
    //记录返回的事件数量
    int number = 0;
    int ret = -1;

    //监听连接请求和子进程退出信号
    while (!m_stop)
    {
        //number记录返回的事件数量
        //等待连接请求或者子进程退出信号
        number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);

        //epoll_wait返回-1且不是EINTR(信号中断)错误
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            //跳出循环
            break;
        }

        //开始遍历接收到的事件
        for (int i = 0; i < number; ++i)
        {
            //获取事件的文件描述符
            int sockfd = events[i].data.fd;
            //判断事件的文件描述符是否为连接请求文件描述符
            if (sockfd == m_listenfd)
            {
                //如果有新连接到来，就采用Round Robin方式将其分配个一个子进程处理
                //获取下一个要分配新连接的子进程的索引
                int i = sub_process_counter;
                //采用轮询的方式将新链接请求分配给子进程处理
                do
                {
                    if (m_sub_process[i].m_pid != -1)
                    {
                        break;
                    }
                    i = (i + 1) % m_process_number;
                } while (i != sub_process_counter);

                //判断子进程是否还在运行
                if (m_sub_process[i].m_pid == -1)
                {
                    //否则跳出上面的while循环
                    m_stop = true;
                    break;
                }

                //更新下一个要分配新连接的子进程的索引
                sub_process_counter = (i + 1) % m_process_number;
                //更新 sub_process_counter 变量的目的是为了确保新连接请求均匀地分配给所有子进程。
                //将new_conn变量发送给子进程
                send(m_sub_process[i].m_pipefd[0], (char *) &new_conn, sizeof(new_conn), 0);
                printf("Send request to child %d\n", i);
            }
            //下面处理父进程收到的信号
            else if ((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                //从管道文件开始读取信号
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if (ret < 0)
                {
                    continue;
                }
                else
                {
                    //开始读取每个信号
                    for (int i = 0; i < ret; ++i)
                    {
                        //开始根据信号的值判断
                        switch (signals[i])
                        {
                                //收到了SIGCHLD信号
                            case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                //返回pid是子进程pid
                                //循环等待子进程退出
                                while ((pid = waitpid(-1, &stat, WNOHANG)) > 0)
                                {
                                    //遍历所有子进程
                                    for (int i = 0; i < m_process_number; ++i)
                                    {
                                        //如果进程池中第i个子进程退出了，则主进程关闭相应的通信管道，并设置相应的m_pid为-1，以标记该子进程已经退出
                                        //判断是否子进程退出
                                        if (m_sub_process[i].m_pid == pid)
                                        {
                                            printf("child %d join\n", i);
                                            close(m_sub_process[i].m_pipefd[0]);
                                            m_sub_process[i].m_pid = -1;
                                        }
                                    }
                                }
                                //如果所有子进程都退出了，则父进程也退出
                                m_stop = true;
                                for (int i = 0; i < m_process_number; ++i)
                                {
                                    if (m_sub_process[i].m_pid != -1)
                                    {
                                        m_stop = false;
                                    }
                                }
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                //如果父进程收到终止信号，那么杀死所有子进程，并等待他们结束
                                printf("kill all the child now!\n");
                                for (int i = 0; i < m_process_number; ++i)
                                {
                                    //获取子进程pid
                                    int pid = m_sub_process[i].m_pid;
                                    //判断子进程是否还在运行
                                    if (pid != -1)
                                    {
                                        //向子进程发送SIGTERM信号
                                        kill(pid, SIGTERM);
                                    }
                                }
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            else
            {
                continue;
            }
        }
    }
    //创建者关闭这个文件描述符
    //    close(m_listenfd);
    close(m_epollfd);
}
#endif//PROCESSPOOL_H_
