//代码清单15-2 CGI服务器


#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// 引用上一节介绍的进程池
#include "processpool.h"

//用于处理客户CGI请求的类，它可以作为processpool类的模板参数
class cgi_conn
{
public:
    cgi_conn() {}

    ~cgi_conn() {}

    //初始化客户连接，清空读缓冲区
    void init(int epollfd, int sockfd, const sockaddr_in &client_addr)
    {
        //将以下三个信息传递给cig_conn对象
        m_epolld = epollfd;
        m_sockfd = sockfd;
        m_address = client_addr;
        memset(m_buf, '\0', BUFFER_SIZE);
        //读缓冲区已经读入的客户数据的最后一个字节的下一个位置
        m_read_idx = 0;
    }

    //处理客户请求
    void process()
    {
        int idx = 0;
        int ret = -1;
        //循环读取和分析客户数据
        while (true)
        {
            idx = m_read_idx;
            ret = recv(m_sockfd, m_buf + idx, BUFFER_SIZE - 1 - idx, 0);
            //如果读操作发生错误，则关闭客户连接，但如果是暂时无数据可读，则退出循环
            if (ret < 0)
            {
                if (errno != EAGAIN)
                {
                    removefd(m_epolld, m_sockfd);
                }
                break;
            }
            //如果对方关闭了连接，则服务器也关闭连接
            else if (ret == 0)
            {
                removefd(m_epolld, m_sockfd);
                break;
            }
            //如果读取到数据，将数据解析为CGI请求
            else
            {
                m_read_idx += ret;
                printf("user content is: %s\n", m_buf);
                //如果遇到字符"\r\n", 则开始处理客户请求
                for (; idx < m_read_idx; ++idx)
                {
                    if ((idx >= 1) && (m_buf[idx - 1] == '\r') && (m_buf[idx] == '\n'))
                    {
                        break;
                    }
                }
                //如果没有遇到字符"\r\n"，则需要读取更多客户数据
                if (idx == m_read_idx)
                {
                    continue;
                }
                m_buf[idx - 1] = '\0';

                char *file_name = m_buf;
                //判断客户要运行的CGI程序是否存在
                //access就是访问某个文件是否存在
                printf("m_buf : %s\n", m_buf);
                if (access(file_name, F_OK) == -1)
                {
                    removefd(m_epolld, m_sockfd);
                    break;
                }
                //如果CGI请求有效
                //创建子进程来执行CGI程序
                ret = fork();
                if (ret == -1)
                {
                    removefd(m_epolld, m_sockfd);
                    break;
                }
                else if (ret > 0)
                {
                    //父进程只需关闭连接
                    removefd(m_epolld, m_sockfd);
                    break;
                }
                else
                {
                    //子进程将标准输出定向到m_sockfd，并执行CGI程序
                    //关闭子进程的标准输出文件描述符
                    close(STDOUT_FILENO);
                    //将客户连接的文件描述符复制到子进程的标准输出文件描述符即子进程的标准输出重定向到客户连接
                    dup(m_sockfd);
                    //执行CGI程序，m_buf 字符串中包含了 CGI 程序的名称。
                    //execl() 函数将替换当前子进程为新的程序。这意味着子进程将不再存在
                    execl(m_buf, m_buf, 0);
                    exit(0);
                }
            }
        }
    }

private:
    //读缓冲区的大小
    static const int BUFFER_SIZE = 1024;
    //epoll实例的文件描述符
    static int m_epolld;
    //客户连接的文件描述符
    int m_sockfd;
    //客户连接的地址信息
    sockaddr_in m_address;
    //读缓冲区
    char m_buf[BUFFER_SIZE];
    //标记读缓冲区中已经读入的客户数据的最后一个字节的下一个位置
    int m_read_idx;
};

int cgi_conn::m_epolld = -1;

int main(int argc, char *argv[])
{

    if (argc <= 2)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd > 0);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    ret = bind(listenfd, (struct sockaddr *) &address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    processpool<cgi_conn> *pool = processpool<cgi_conn>::create(listenfd);
    if (pool)
    {
        pool->run();
        delete pool;
    }
    //如前所说，main函数创建的listefd，自己关闭
    close(listenfd);


    return 0;
}