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