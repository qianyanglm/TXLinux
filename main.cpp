//代码清单13-4,自己照着写的，千万不要取代掉
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
#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define USER_LIMIT 5
#define BUFFER_SIZE 1024
#define FD_LIMIT 65535
#define MAX_EVENT_NUMBER 1024
#define PROCESS_LIMIT 65536

//处理一个客户连接必要的数据
struct client_data
{
    sockaddr_in address;//客户端的socket地址
    int connfd;         //socket文件描述符
    pid_t pid;          //处理这个连接的子进程的PID
    int pipefd[2];      //和父进程通信用的管道
};

static const char *shm_name = "/my_shm";
int sig_pipefd[2];
int epollfd;
int listenfd;
int shmfd;
char *share_mem = 0;
//客户连接数组，进程使用客户连接的编号来索引这个数组，即可以取得相关的客户连接数据
client_data *users = 0;
//子进程和客户连接的映射关系表，用进程的PID来索引这个数组，即可取得该进程所处理的客户连接的编号
int *sub_process = 0;
//当前客户数量
int user_count = 0;
bool stop_child = false;

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
void addfd(int eollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    //当该文件描述符上有数据可读时，将触发epoll事件
    event.events = EPOLLIN | EPOLLET;
    //向事件表注册fd上的事件
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //调用上面函数将文件描述符设置为非阻塞，以确保后续的I/O操作不会阻塞整个程序
    setnonblocking(fd);
}

//信号处理函数
void sig_handler(int sig)
{
    //保留原来的errno,在函数最后恢复，以保证函数的可重入性
    int save_errno = errno;
    int msg = sig;
    //将信号写入管道,以通知主循环
    send(sig_pipefd[1], (char *) &msg, 1, 0);
    errno = save_errno;
}

//注册信号处理函数
void addsig(int sig, void (*handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    //系统调用被信号中断时，将自动重启
    if (restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    //设置为包含所有信号的信号集
    sigfillset(&sa.sa_mask);
    //注册信号处理函数，失败的话将引发断言错误
    assert(sigaction(sig, &sa, NULL) != -1);
}

void del_resource()
{
    close(sig_pipefd[0]);
    close(sig_pipefd[1]);
    close(listenfd);
    close(epollfd);
    shm_unlink(shm_name);
    delete[] users;
    delete[] sub_process;
}

//停止一个子进程
void child_term_handler(int sig)
{
    stop_child = true;
}

//子进程运行的函数，参数idx指出该子进程处理的客户连接的编号,users是保存所有客户连接数据的数组，参数share_mem指出共享内存的起始地址
int run_child(int idx, client_data *users, char *share_mem)
{
    epoll_event events[MAX_EVENT_NUMBER];
    //子进程使用I/O复用技术来同时监听两个文件描述符：客户连接socket、与父进程通心的管道文件描述符
    int child_epollfd = epoll_create(5);
    assert(child_epollfd != -1);
    int connfd = users[idx].connfd;
    addfd(child_epollfd, connfd);
    int pipefd = users[idx].pipefd[1];
    addfd(child_epollfd, pipefd);
    int ret;
    //子进程需要设置自己的信号处理函数
    addsig(SIGTERM, child_term_handler, false);

    while (!stop_child)
    {
        int number = epoll_wait(child_epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure!\n");
            break;
        }

        for (int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            //本子进程负责的客户连接有数据到达
            if ((sockfd == connfd) && (events[i].events & EPOLLIN))
            {
                memset(share_mem + idx * BUFFER_SIZE, '\0', BUFFER_SIZE);
                //将客户数据读取到对应的读缓存中，该读缓存是共享内存的一段，它开始于idx*BUFFER_SIZE处，长度为BUFFER_SIZE字节，因此各个客户连接的读缓存是共享的
                ret = recv(connfd, share_mem + idx * BUFFER_SIZE, BUFFER_SIZE - 1, 0);
                if (ret < 0)
                {
                    if (errno != EAGAIN)
                    {
                        stop_child = true;
                    }
                }
                else if (ret == 0)
                {
                    stop_child = true;
                }
                else
                {
                    //成功读取客户数据后就通知主进程(通过管道)来处理
                    send(pipefd, (char *) &idx, sizeof(idx), 0);
                }
            }
            //主进程通知本进程（通过管道）将低client个客户的数据发送到本进程负责的客户端
            else if ((sockfd == pipefd) && (events[i].events & EPOLLIN))
            {
                int client = 0;
                //接收主进程发送来的数据，即有客户数据到达的连接的编号
                ret = recv(sockfd, (char *) client, sizeof(client), 0);
                if (ret < 0)
                {
                    if (errno != EAGAIN)
                    {
                        stop_child = true;
                    }
                }
                else if (ret == 0)
                {
                    stop_child = true;
                }
                else
                {
                    send(connfd, share_mem + client * BUFFER_SIZE, BUFFER_SIZE, 0);
                }
            }
            else
            {
                continue;
            }
        }
    }

    close(connfd);
    close(pipefd);
    close(child_epollfd);
    return 0;
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

    listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    ret = bind(listenfd, (struct sockaddr *) &address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    user_count = 0;
    users = new client_data[USER_LIMIT + 1];
    sub_process = new int[PROCESS_LIMIT * 10];
    for (int i = 0; i < PROCESS_LIMIT; ++i)
    {
        sub_process[i] = -1;
    }

    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd, listenfd);

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
    assert(ret != -1);
    setnonblocking(sig_pipefd[1]);
    addfd(epollfd, sig_pipefd[0]);

    addsig(SIGCHLD, sig_handler);
    addsig(SIGTERM, sig_handler);
    addsig(SIGINT, sig_handler);
    addsig(SIGPIPE, SIG_IGN);
    bool stop_server = false;
    bool terminate = false;


    //创建共享内存，作为所有客户socket连接的读缓存
    shmfd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    assert(shmfd != -1);
    ret = ftruncate(shmfd, USER_LIMIT * BUFFER_SIZE);
    assert(ret != -1);

    share_mem = (char *) mmap(NULL, USER_LIMIT * BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
    assert(share_mem != MAP_FAILED);
    close(shmfd);

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
            //新的客户连接到来
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *) &client_address, &client_addrlength);
                if (connfd < 0)
                {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                if (user_count >= USER_LIMIT)
                {
                    const char *info = "too many users\n";
                    printf("user_count: %d\n", user_count);
                    printf("%s", info);
                    send(connfd, info, strlen(info), 0);
                    close(connfd);
                    continue;
                }
                //保存第user_count个客户连接的相关数据
                users[user_count].address = client_address;
                users[user_count].connfd = connfd;
                //在主进程和子进程间建立管道，以传递必要的数据
                ret = socketpair(PF_UNIX, SOCK_STREAM, 0, users[user_count].pipefd);
                assert(ret != -1);
                pid_t pid = fork();
                if (pid < 0)
                {
                    close(connfd);
                    continue;
                }
                else if (pid == 0)
                {
                    close(epollfd);
                    close(listenfd);
                    close(users[user_count].pipefd[0]);
                    close(sig_pipefd[0]);
                    close(sig_pipefd[1]);
                    run_child(user_count, users, share_mem);
                    munmap((void *) share_mem, USER_LIMIT * BUFFER_SIZE);
                    exit(0);
                }
                else
                {
                    close(connfd);
                    close(users[user_count].pipefd[1]);
                    addfd(epollfd, users[user_count].pipefd[0]);
                    users[user_count].pid = pid;
                    //记录新的客户连接在数组users中的索引值，建立进程pid和该索引值之间的映射关系
                    sub_process[pid] = user_count;
                    user_count++;
                }
            }
            //处理信号事件
            else if ((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
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
                            //子进程退出，表示有某个客户端关闭了连接
                            case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                while ((pid == waitpid(-1, &stat, WNOHANG)) > 0)
                                {
                                    //用子进程的pid取得被关闭的客户连接的编号
                                    int del_user = sub_process[pid];
                                    sub_process[pid] = -1;
                                    if ((del_user < 0) || (del_user > USER_LIMIT))
                                    {
                                        continue;
                                    }
                                    //清楚第del_user个客户连接使用的相关数据
                                    epoll_ctl(epollfd, EPOLL_CTL_DEL, users[del_user].pipefd[0], 0);
                                    close(users[del_user].pipefd[0]);
                                    users[del_user] = users[--user_count];
                                    sub_process[users[del_user].pid] = del_user;
                                }
                                if (terminate && user_count == 0)
                                {
                                    stop_server = true;
                                }
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                //结束服务器程序
                                printf("kill all the clild now\n");
                                if (user_count == 0)
                                {
                                    stop_server = true;
                                    break;
                                }
                                for (int i = 0; i < user_count; ++i)
                                {
                                    int pid = users[i].pid;
                                    kill(pid, SIGTERM);
                                }
                                terminate = true;
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
            //某个子进程向父进程写入了数据
            else if (events[i].events & EPOLLIN)
            {
                int child = 0;
                //读取管道数据，child变量记录了是哪个客户连接有数据到达
                ret = recv(sockfd, (char *) &child, sizeof(child), 0);
                printf("read data from child accross pipe\n");
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    //向除负责处理第child个客户连接的子进程之外的其他子进程发送消息，通知他们有客户数据要写
                    for (int j = 0; j < user_count; ++j)
                    {
                        if (users[j].pipefd[0] != sockfd)
                        {
                            printf("send data to child accross pipe\n");
                            send(users[j].pipefd[0], (char *) &child, sizeof(child), 0);
                        }
                    }
                }
            }
        }
    }

    del_resource();
    return 0;
}
