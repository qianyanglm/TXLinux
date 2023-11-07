//
// Created by A qian yang on 2023/11/6.
//
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


#endif//PROCESSPOOL_H_
