//
// Created by A qian yang on 2023/11/9.
//
#include "http_conn.h"
#include <sys/uio.h>

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
void modfd(int epollfd, int fd, int ev)
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
//这段代码之所以要判断两次，是因为 HTTP 协议规定，请求行的结尾可以是 \r\n 或 \n，所以多了一个逻辑来判断\n情况
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
                //设置为空字符，表示请求行的结束
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
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
                //设置为空字符，表示请求行的结束。
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }

            //如果换行符前面没有回车符，则表示请求行解析失败，返回 LINE_BAD 状态。
            return LINE_BAD;
        }
    }
    //如果循环结束，则表示请求行还没有解析完，返回 LINE_OPEN 状态。
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read()
{
    //如果读缓冲区已满，返回false
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }

    int bytes_read = 0;
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1)
        {
            //表示读取数据没有准备好，可以稍后再试。
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            return false;
        }
        //客户端关闭了连接
        else if (bytes_read == 0)
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

//解析HTTP请求行，获得请求方法、目标URL，以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //返回text中匹配" \t"的参数
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char *method = text;
    //忽略大小写比较字符串相同返回0
    if (strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else
    {
        return BAD_REQUEST;
    }

    //返回m_url中第一个不在" \t"中出现的字符下标
    m_url += strspn(m_url, " \t");
    //返回m_url中匹配" \t"的参数
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }
    //忽略大小写比较前7个字符
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        //查找返回'/'出现的位置
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //遇到空行，表示头部字段解析完毕
    if (text[0] == '\0')
    {
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        //否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    //处理Connection头部字段
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    //处理Content-Length头部字段
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    //处理Host头部字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        printf("oop! unknow header %s\n", text);
    }

    return NO_REQUEST;
}

//未真正解析HTTP请求的消息体，只是判断它是否被完整地读入了
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    //适用于GET请求，因为GET请求的请求体长度是固定的
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

//主状态机。
http_conn::HTTP_CODE http_conn::process_read()
{

    //存储行解析的状态
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    //存储正在解析的文本
    char *text = 0;

    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status == parse_line()) == LINE_OK))
    {
        //获取一行文本
        text = get_line();
        m_start_line = m_checked_idx;
        printf("got 1 http line: %s\n", text);

        //根据状态解析HTTP请求
        switch (m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if (ret == GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST)
                {
                    return do_request();
                }
                //如果请求体解析失败设置为LINE_OPEN表示没有解析完毕
                line_status = LINE_OPEN;
                break;
            }
                //如果不是这三种状态就报错
            default:
            {
                return INTERAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

//当得到一个完整、正确得到HTTP请求时，我们就分析目标文件的属性，如果目标文件存在、对所有用户可读，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    //把doc_root指向的字符串复制到m_real_file中
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //将m_url剩余的字符串追加到m_real_file末尾得到目标文件的完整路径
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    //获取文件属性
    if (stat(m_real_file, &m_file_stat) < 0)
    {
        return NO_REQUEST;
    }
    //如果文件不存在
    if (!(m_file_stat.st_mode & S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }
    //如果文件不可读
    if (S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST;
    }
    //只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    //将文件映射到内存
    m_file_address = (char *) mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    //关闭文件
    close(fd);
    return FILE_REQUEST;
}

//堆内存映射区执行munmap操作
void http_conn::unmap()
{
    if (m_file_address)
    {
        //释放文件映射区
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//写http响应
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    while (1)
    {
        //将多块内存数据一并写入文件描述符中
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1)
        {
            //如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件。虽然在此期间，服务器无法立即接收到同一个客户的下一个请求，但是可以保证连接的完整性
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if (bytes_to_send <= bytes_have_send)
        {
            //发送HTTP响应成功，根据HTTP请求中的Conection字段决定是否立即关闭连接
            unmap();
            if (m_linger)
            {
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            else
            {
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return false;
            }
        }
    }
}

//往写缓冲中写入待发送的数据
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
}