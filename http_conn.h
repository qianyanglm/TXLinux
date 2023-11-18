//
// Created by A qian yang on 2023/11/9.
//
#ifndef HTTP_CONN_H_
#define HTTP_CONN_H_

#include "locker.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

class http_conn
{
public:
    //文件名的最大长度
    static const int FILENAME_LEN = 200;
    //读缓冲区的大小
    static const int READ_BUFFER_SIZE = 2048;
    //写缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;

    //HTTP请求方法，但我们仅支持GET
    enum METHOD { GET = 0,//获取资源
                  POST,   //传输实体主体
                  HEAD,   //获取报文头部
                  PUT,    //上传文件
                  DELETE, //删除文件
                  TRACE,  //追踪路径
                  OPTIONS,//询问支持的方法
                  CONNECT,//要求用隧道协议连接代理
                  PATCH };//对资源进行部分修改

    //解析客户请求时，主状态机所处的状态
    enum CHECK_STATE {
        //正在分析请求行
        CHECK_STATE_REQUESTLINE = 0,
        //正在分析请求头
        CHECK_STATE_HEADER,
        //正在分析请求内容
        CHECK_STATE_CONTENT
    };

    //服务器处理HTTP请求的可能结果
    enum HTTP_CODE { NO_REQUEST,       //客户端没有发送任何请求
                     GET_REQUEST,      //客户端发送了GET请求
                     BAD_REQUEST,      //客户端发送了无效的请求
                     NO_RESOURCE,      //请求的资源不存在
                     FORBIDDEN_REQUEST,//客户端没有访问资源的权限
                     FILE_REQUEST,     //请求的资源是文件
                     INTERAL_ERROR,    //服务器内部错误
                     CLOSED_CONNECTION //客户端关闭了连接
    };

    //行的读取状态
    enum LINE_STATUS { LINE_OK = 0,//行读取完毕
                       LINE_BAD,   //行读取失败
                       LINE_OPEN   //行还没有读取完毕
    };

public:
    http_conn();
    ~http_conn();

public:
    //初始化新接受的连接
    void init(int sockfd, const sockaddr_in &addr);
    //关闭连接
    void close_conn(bool real_close = true);
    //处理客户请求
    void process();
    //非阻塞读操作
    bool read();
    //非阻塞写操作
    bool write();

private:
    //初始化连接
    void init();
    //解析HTTP请求
    HTTP_CODE process_read();
    //填充HTTP应答
    bool process_write(HTTP_CODE ret);


    //下面一组函数被process_read函数调用以分析HTTP请求
    //解析请求行
    HTTP_CODE parse_request_line(char *text);
    //解析请求头
    HTTP_CODE parse_headers(char *text);
    //解析请求体
    HTTP_CODE parse_content(char *text);
    //根据请求方法来处理请求
    HTTP_CODE do_request();

    //获取一行文本
    char *get_line() { return m_read_buf + m_start_line; }

    //解析一行文本
    LINE_STATUS parse_line();

    //下面这一组函数被process_write调用以填充HTTP应答
    //释放指向的内存
    void unmap();
    //添加HTTP函数用于添加HTTP应答头或正文
    bool add_response(const char *format, ...);
    //添加HTTP正文
    bool add_content(const char *content);
    //添加HTTP状态行
    bool add_status_line(int status, const char *title);
    //添加HTTP头部，`Content-Type: text/html` 和 `Content-Length
    bool add_headers(int content_length);
    //添加HTTP头部，Connection: keep-alive
    bool add_content_length(int content_length);
    bool add_linger();
    //添加HTTP空行
    bool add_blank_line();


public:
    //所有socket上的事件都被注册到同一个epoll内核事件表中，所以将epoll文件描述符设置为静态的
    static int m_epollfd;
    //统计用户数量
    static int m_user_count;

private:
    //该HTTP连接的socket和对方的socket地址
    int m_sockfd;
    sockaddr_in m_address;

    //读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];
    //标识读缓冲区中已经读入的客户数据的最后一个字节的下一个位置
    int m_read_idx;
    //当前正在分析的字符在读缓冲区中的位置
    int m_checked_idx;
    //当前正在解析的行的起始位置
    int m_start_line;
    //写缓冲区
    char m_write_buf[WRITE_BUFFER_SIZE];
    //写缓冲区中待发送的字节数
    int m_write_idx;

    //主状态机当前所处的状态
    CHECK_STATE m_check_state;
    //请求方法
    METHOD m_method;

    //客户请求的目标文件的完整路径，其内容等于doc_root+m_rul,doc_root是网站根目录
    char m_real_file[FILENAME_LEN];
    //客户请求的目标文件的文件名
    char *m_url;
    //HTTP协议版本号，只支持HTTP/1.1
    char *m_version;
    //主机名
    char *m_host;
    //HTTP请求的消息体的长度
    int m_content_length;
    //HTTP请求是否要求保持连接
    bool m_linger;


    //客户请求的目标文件被mmap到内存中的起始位置
    char *m_file_address;
    //目标文件的状态.通过它我们可以判断文件是否存在，是否为目录，是否可读，并获取文件大小等信息
    struct stat m_file_stat;
    //我们将采用writev来执行写操作，所以定义下面两个成员
    struct iovec m_iv[2];
    //被写内存快数量
    int m_iv_count;
};


#endif//HTTP_CONN_H_
