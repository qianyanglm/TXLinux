//
// Created by A qian yang on 2023/11/9.
//
#include "http_conn.h"

//定义HTTP响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inhernetly impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the requested file.\n";
//网站根目录
const char *doc_root = "/var/ww/html";

//把文件读写模式设置为非阻塞的
int setnonblocking(int fd)
{
    //获取文件描述符当前的标志状态
    int old_option = fcntl(fd, F_GETFL);
    //在old_option的基础上进行位或运算，设置非阻塞标志
    fcntl(fd, F_SETFL, old_option | O_NONBLOCK);
    //返回文件描述符老的状态
    return old_option;
}

//向epoll实例添加文件描述符
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    //监听读事件和ET模式
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    //fd添加到epoll实例epollfd监听的描述符集合中
    if (one_shot)
    {
        //若为真则使用一次性触发模式
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //fd设置为非阻塞模式
    setnonblocking(fd);
}

//从epoll实例中移除文件描述符fd,并关闭文件描述符fd
void removefd(int epollfd, int fd)
{
    //将fd从epoll实例中移除
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    //关闭fd
    close(fd);
}

//对指定在epoll中的fd注册进行修改
void mofd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    //EPOLLET: 设置为边缘触发(Edge Triggered)模式
    //EPOLLONESHOT: 只监听一次事件,当监听完这次事件之后,如果还需要继续监听这个socket的话,需要再次把这个socket加入到EPOLL队列里
    //EPOLLRDHUP: 当socket连接断开时,会收到事件通知
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    //修改fd上的注册事件
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接
void http_conn::close_conn(bool real_close)
{
    //为true且m_sockfd文件描述符有效
    if (real_close && (m_sockfd != -1))
    {
        //从epoll中删除m_sockfd
        removefd(m_epollfd, m_sockfd);
        //表示无效文件描述符
        m_sockfd = -1;
        //关闭一个连接时，将客户总量-1
        m_user_count--;
    }
}

//初始化新接受的连接
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //如下两行是为了避免TIME_WAIT状态，仅用于调试，实际使用时应该去掉
    int reuse = 1;
    //设置socket选项SO_REUSEADDR
    //SO_REUSEADDR选项的作用是允许重用本地地址和端口,通常在服务器端listen的socket上设置该选项,以允许服务器快速重启,即使之前的连接还未完全关闭也能绑定到同一个端口
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    //在epoll中注册该连接socket的读事件
    addfd(m_epollfd, sockfd, true);
    //用户计数+1
    m_user_count++;

    //调用私有方法进行其他初始化工作
    init();
}

//初始化HTTP连接
void http_conn::init()
{
    //开始检查请求行
    m_check_state = CHECK_STATE_REQUESTLINE;
    //不使用长连接
    m_linger = false;

    //设置为GET，表示默认请求方法GET
    m_method = GET;
    //将以下初始化为0
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    //将所有元素初始化为'\0'即0
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机,用于解析HTTP请求行
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        //如果遇到回车符
        if (temp == '\r')
        {
            //如果回车符后面没有换行符，则表示请求行还没有解析完，返回 LINE_OPEN 状态
            if ((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            //如果回车符后面有换行符，则表示请求行解析完毕，返回 LINE_OK 状态。
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return lINE_OK;
            }

            //如果遇到其他字符，则表示请求行解析失败，返回 LINE_BAD 状态。
            return LINE_BAD;
        }
        //如果遇到换行符
        else if (temp == '\n')
        {
            //如果换行符前面有回车符，则表示请求行解析完毕，返回 LINE_OK 状态
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r'))
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return lINE_OK;
            }

            //如果换行符前面没有回车符，则表示请求行解析失败，返回 LINE_BAD 状态。
            return LINE_BAD;
        }
    }
    //如果循环结束，则表示请求行还没有解析完，返回 LINE_OPEN 状态。
    return LINE_OPEN;
}