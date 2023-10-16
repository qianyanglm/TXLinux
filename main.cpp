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