#include <arpa/inet.h>
#include <assert.h>
#include <climits>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>//shm_open,shm_unlink
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BUFFER_SIZE 64
#define USER_LIMIT 5
#define FD_LIMIT 65535
#define MAX_EVENT_NUMBER 1024
#define PROCESS_LIMIT 65535

int sig_pipefd[2];
bool stop_child = false;//子进程的标志位

struct client_data
{
    sockaddr_in address;//客户端的socket地址
    int connfd;         //socket文件描述符
    pid_t pid;          //处理这个连接的子进程PID
    int pipefd[2];      //和父进程通信用的管道
};

//把文件的读写模式设置为非阻塞的
int setnoblocking(int fd)
{
    //获取文件描述符当前的标志状态
    int old_option = fcntl(fd, F_GETFL);
    //在old_option的基础上进行位或运算，设置非阻塞标志
    fcntl(fd, F_SETFL, old_option | O_NONBLOCK);
    //返回文件描述符老的状态
    return old_option;
}

//向epoll实例添加一个文件描述符
void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    //监听读事件和ET模式
    event.events = EPOLLIN | EPOLLET;
    //fd添加到epoll实例epollfd监听的描述符集合中
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //fd设置为非阻塞模式
    setnoblocking(fd);
}

//信号处理函数使用管道把信号传递给主循环
void sig_handler(int sig)
{
    //保留原来的errno，在函数最后恢复，以保证可重入性。
    int save_errno = errno;
    int msg = sig;
    //发送msg到管道的写端，将信号值传递出去
    send(sig_pipefd[1], (void *) &msg, 1, 0);
    //恢复原来的信号值
    errno = save_errno;
}

//信号处理函数的实现
//void (*handler)(int) 表示 接受一个int类型参数，返回值为void的函数指针handler
void addsig(int sig, void (*handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;//指定信号处理函数
    if (restart)
        sa.sa_flags |= SA_RESTART;//为true则重启系统调用
    //在信号集中设置所有信号，即将所有信号设置为1，阻塞所有信号
    sigfillset(&sa.sa_mask);
    //捕获到信号sig，使用sa中的信号处理函数处理
    assert(sigaction(sig, &sa, NULL) != -1);
}

//关掉子进程
void child_term_handler(int sig)
{
    stop_child = true;
}

/*子进程运行的函数
idx表示该子进程处理的客户连接的编号
users是保存所有客户连接数据的数组
share_mem指出共享内存的起始地址
子进程使用epoll来同时监听两个文件描述符：
    客户连接socket
    与父进程(主进程)通信的管道文件描述符
*/
int run_child(int idx, client_data *users, char *share_mem)
{
    epoll_event events[MAX_EVENT_NUMBER];
    int child_epollfd = epoll_create(5);
    assert(child_epollfd != -1);
    printf("child pid=%d is running\n", getpid());
    //获取当前子进程需要处理的客户连接connfd
    int connfd = users[idx].connfd;
    //将connfd添加到子进程实例中，以便监听该客户端套接字
    addfd(child_epollfd, connfd);
    int pipefd = users[idx].pipefd[1];
    //同上，以便于主进程通信
    addfd(child_epollfd, pipefd);

    //设置进程被终止时的信号处理函数
    addsig(SIGTERM, child_term_handler, false);

    int ret;

    while (!stop_child)
    {
        //number表示有多少个网络事件发生
        int number = epoll_wait(child_epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            /*EINTR是linux中函数的返回状态，在不同的函数中意义不同。
            表示某种阻塞的操作，被接收到的信号中断，造成的一种错误返回值。*/
            printf("epoll failure\n");
            break;
        }
        //遍历events的每个事件
        for (int i = 0; i < number; i++)
        {
            //发生事件的socket
            int sockfd = events[i].data.fd;
            //本子进程负责的socket有数据进来
            if ((sockfd == connfd) && (events[i].events & EPOLLIN))
            {
                //把本进程对应的共享内存段清空
                memset(share_mem + idx * BUFFER_SIZE, '\0', BUFFER_SIZE);
                //读取数据
                ret = recv(sockfd, share_mem + idx * BUFFER_SIZE, BUFFER_SIZE - 1, 0);

                //我添加的打印语句，这里不太合适，出现的太早了
                printf("Received %d bytes from client %d: %s", ret, i, share_mem + idx * BUFFER_SIZE);

                if (ret < 0)
                {
                    if (errno != EAGAIN)
                        stop_child = true;
                }
                //客户端断开
                else if (ret == 0)
                {
                    //标记是否退出循环
                    stop_child = true;
                    //打印信息来通知主进程
                    printf("\nget nothing1\n");

                    //我自己加的，每次一个客户端连接断开的时候，显示现在还有几个连接
                    printf("a client left\n");
                    //我自己加了这句话，来提示有几个连接
                    printf("leaves a user, now have %d users\n", idx);

                    continue;
                }
                //如果正常读取数据的话
                else
                {
                    //通过pipe发送idx给主进程，idx表示第几个客户端
                    assert(send(pipefd, (void *) &idx, sizeof(idx), 0) != -1);//这里可以尝试一下改成void会怎么样
                }
            }
            //主进程通知本进程，将第client个客户的数据发送到本进程负责的客户端
            else if ((sockfd == pipefd) && (events[i].events & EPOLLIN))
            {
                int client = 0;
                //从共享内存中读出数据
                ret = recv(sockfd, (void *) &client, sizeof(client), 0);
                if (ret < 0)
                {
                    if (errno != EAGAIN)
                        stop_child = true;
                }
                else if (ret == 0)
                    stop_child = true;
                else
                {
                    //发送给指定客户端
                    send(connfd, share_mem + client * BUFFER_SIZE, BUFFER_SIZE, 0);
                }
            }
            else
            {
                printf("unprocessed circumstance\n");
            }
        }
    }
    close(connfd);
    close(pipefd);
    close(child_epollfd);//根据mam手册上的描述，该行可以省略
    return 0;
}

int main(int argc, char const *argv[])
{
    if (argc <= 2)
    {
        printf("usags ip_addreee port_number\n");
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);

    static const char *shm_name = "/my_shm";

    int listenfd;
    int *sub_prcess = nullptr;//子进程和客户连接的映射关系表。用进程pid索引这个数组，取得该进程处理的客户连接的编号

    //初始化users以及hash表
    int user_count = 0;//当前的客户数量
    client_data *users = new client_data[USER_LIMIT + 1];
    //客户连接数组。进程用客户连接的编号来索引这个数组，即可取得相关的客户连接数据
    //这里必须*10，因为进程pid很大概率会大于65535，以前可以运行，现在不行了
    sub_prcess = new int[PROCESS_LIMIT * 10];
    for (int i = 0; i < PROCESS_LIMIT; i++)
    {
        sub_prcess[i] = -1;
    }

    //socket的一套，建立，绑定，监听
    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, ip, &address.sin_addr);

    listenfd = socket(PF_INET, SOCK_STREAM, 0);//创建套接字
    assert(listenfd >= 0);

    ret = bind(listenfd, (struct sockaddr *) &address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5);//backlog就设成5
    assert(ret != -1);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd, listenfd);

    //使用socketpair创建管道，注册sig_pipefd[0]上的可读事件
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);//socket用于本地通信
    assert(ret != -1);
    setnoblocking(sig_pipefd[1]);
    addfd(epollfd, sig_pipefd[0]);

    //设置一些信号的处理函数
    //#define	SIG_IGN	 ((__sighandler_t)  1)	/* Ignore signal.  */
    addsig(SIGPIPE, SIG_IGN);    //SIGPIPE往读端被关闭的管道或者socket写数据
    addsig(SIGCHLD, sig_handler);//SIGCHLD子进程状态发生变化，退出或者暂停
    addsig(SIGTERM, sig_handler);//SIGTERM，终止进程，kill命令默认发送的信号
    addsig(SIGINT, sig_handler); //SIGINT键盘输入以中断进程
    bool stop_server = false;
    bool terminate = false;

    /*创建共享内存，作为所有客户socket连接的读缓存
        O_RDWR     Open the object for read-write access.
        O_CREAT    Create the shared memory object if it does not  exist.
    */
    //shm_open打开一个共享内存对象,shm_name指定名字,有O_CREAT表示创建,0666设定权限。
    int shmfd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    // A new shared memory object  initially  has  zero  length—the size of the object can be set using ftruncate(2).
    assert(shmfd != -1);
    //ftruncate设置共享内存大小,这里是 USER_LIMIT * BUFFER_SIZE 字节。
    ret = ftruncate(shmfd, USER_LIMIT * BUFFER_SIZE);
    assert(ret != -1);

    //开辟共享内存，建立映射，然后关掉
    //mmap将共享内存映射到进程地址空间,NULL表示自动选择地址,后面是指定可读写,以及映射方式为共享。
    char *share_mem = (char *) mmap(NULL, USER_LIMIT * BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
    assert(share_mem != MAP_FAILED);
    close(shmfd);


    while (!stop_server)
    {
        //获取活跃事件
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            /*EINTR是linux中函数的返回状态，在不同的函数中意义不同。
            表示某种阻塞的操作，被接收到的信号中断，造成的一种错误返回值。*/
            printf("epoll failure\n");
            break;
        }
        for (int i = 0; i < number; i++)
        {

            int sockfd = events[i].data.fd;
            //新的客户连接到来
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(sockfd, (struct sockaddr *) &client_address, &client_addrlength);
                if (connfd < 0)
                {
                    printf("errno is :%d\n", errno);
                    continue;
                }
                if (user_count >= USER_LIMIT)
                {
                    const char *info = "too many users,please wait others disconnected.\n";
                    printf("%s", info);
                    send(connfd, info, strlen(info), 0);
                    close(connfd);
                    continue;
                }
                //保存客户相关数据
                users[user_count].address = client_address;
                users[user_count].connfd = connfd;

                //在主进程和子进程之间建立双向管道传输数据，可读又可写
                ret = socketpair(PF_UNIX, SOCK_STREAM, 0, users[user_count].pipefd);
                assert(ret != -1);
                pid_t pid = fork();
                //fork出错
                if (pid < 0)
                {
                    printf("fork() error\n");
                    close(connfd);
                    continue;
                }
                //子进程
                else if (pid == 0)
                {
                    //关闭子进程不需要的文件描述符:epollfd、listenfd、主进程端pipe读端、信号管道。
                    close(epollfd);
                    close(listenfd);
                    close(users[user_count].pipefd[0]);
                    close(sig_pipefd[0]);
                    close(sig_pipefd[1]);

                    //子进程处理事件的主循环
                    run_child(user_count, users, share_mem);
                    //释放共享内存空间
                    munmap((void *) share_mem, USER_LIMIT * BUFFER_SIZE);
                    //退出子进程
                    exit(0);
                }
                //父进程，>0就是父进程
                else
                {

                    close(connfd);
                    close(users[user_count].pipefd[1]);
                    addfd(epollfd, users[user_count].pipefd[0]);
                    printf("this is father pid=%d,there are %d users\n", getpid(), user_count);
                    users[user_count].pid = pid;
                    sub_prcess[pid] = user_count;//建立hash表
                    user_count++;
                }
            }
            //处理信号事件
            else if ((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, 1024, 0);
                if (ret == -1)
                {
                    printf("recv error\n");
                    continue;
                }
                else if (ret == 0)
                {
                    printf("get nothing2\n");
                    continue;
                }
                else
                {
                    //每个信号占一个字符，按字节逐个处理接收信号。
                    for (int i = 0; i < ret; i++)
                    {
                        switch (signals[i])
                        {
                            //子进程退出，表示某个客户端关闭了连接
                            case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                //-1     meaning wait for any child process.
                                while ((pid = waitpid(-1, &stat, WNOHANG)) > 0)
                                {
                                    //用子进程的pid取得被关闭的连接的编号
                                    int del_user = sub_prcess[pid];
                                    sub_prcess[pid] = -1;
                                    if ((del_user < 0) || (del_user > USER_LIMIT))
                                    {
                                        continue;
                                    }
                                    epoll_ctl(epollfd, EPOLL_CTL_DEL, users[del_user].pipefd[0], 0);
                                    close(users[del_user].pipefd[0]);
                                    users[del_user] = users[--user_count];
                                    sub_prcess[users[del_user].pid] = del_user;
                                }
                            }
                                if (terminate && user_count == 0)
                                {
                                    stop_server = true;
                                }
                            case SIGHUP:
                            {
                                continue;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                //结束服务器程序
                                printf("killing all the child\n");
                                if (user_count == 0)
                                {
                                    stop_server = true;
                                    break;
                                }
                                for (int i = 0; i < user_count; i++)
                                {
                                    int pid = users[i].pid;
                                    kill(pid, SIGTERM);
                                }
                                terminate = true;
                                //这里不需要把stop_server置1，在断开连接的sig里面处理
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
            //子进程向父进程写入了数据
            else if (events[i].events & EPOLLIN)
            {
                int child = 0;
                ret = recv(sockfd, (void *) &child, sizeof(child), 0);
                printf("read data from child: %d across pipe；ret = %d\n\n", child, ret);

                /*无法输出收到的数据，就很奇怪。
                char recv_data[1024];
                memset(recv_data, '\0', 1024);
                recv(sockfd, recv_data, sizeof(recv_data), 0);
                printf("Received data: %s\n", recv_data);
*/

                if (ret == -1)
                {
                    printf("recv error\n");
                    continue;
                }
                else if (ret == 0)
                {
                    printf("get nothing3\n");
                    continue;
                }
                else
                {
                    for (int j = 0; j < user_count; j++)
                    {
                        if (users[j].pipefd[0] != sockfd)
                        {
                            printf("send data to child accross pipes\n");
                            send(users[j].pipefd[0], (void *) &child, sizeof(child), 0);
                        }
                    }
                }
            }
        }
    }
    close(sig_pipefd[0]);
    close(sig_pipefd[1]);
    close(epollfd);
    close(listenfd);
    shm_unlink(shm_name);
    return 0;
}
