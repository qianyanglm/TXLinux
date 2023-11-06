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
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const int CONTROL_LEN = CMSG_LEN(sizeof(int));

//发送文件描述符
void send_fd(int fd, int fd_to_send)
{
}