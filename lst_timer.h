//代码清单11-2 升序定时器链表
#ifndef LST_TIMER
#define LST_TIMER

#include <bits/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <time.h>
#define BUFFER_SIZE 64
class util_timer;//前向声明

//用户数据结构: 客户端socket地址、socket文件描述符、读缓存和定时器
struct client_data
{
    sockaddr_in address;
    int sockfd;
    char buf[BUFFER_SIZE];
    util_timer *timer;
};

//定时器类
class util_timer
{
public:
    util_timer(): prev(NULL), next(NULL) {}

public:
    time_t expire;                 //任务的超时时间，这里使用绝对时间
    void (*cb_func)(client_data *);//任务回调函数
    //回调函数处理的客户数据，由定时器的执行者传递给回调函数
    client_data *user_data;
    util_timer *prev;//指向前一个定时器
    util_timer *next;//指向下一个定时器
};

//定时器链表。它是一个升序、双向链表，且带有头节点和尾结点
class sort_timer_lst
{
public:
    sort_timer_lst(): head(NULL), tail(NULL) {}

    //链表被销毁时，删除其中所有的定时器
    ~sort_timer_lst()
    {
        util_timer *tmp = head;
        while (tmp)
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    //将目标定时器timer添加到链表中
    void add_timer(util_timer *timer)
    {
        if (!timer)//如果为空指针则返回
        {
            return;
        }
        if (!head)//如果头指针为空
        {
            head = tail = timer;
            return;
        }

        //如果目标定时器可以作为作为链表的头节点
        if (timer->expire < head->expire)
        {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        //如果不可以，就调用重载函数
        add_timer(timer, head);
    }

    //超时时间延长，调整定时器往链表尾部移动
    void adjust_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        util_timer *tmp = timer->next;
        //如果超时了也不用调整
        //被调整的目标定时器在链表尾部，或者该定时器新的超时值仍小于下一个定时器的超时值
        if (!tmp || (timer->expire < tmp->expire))
        {
            return;
        }
        //如果目标定时器是链表的头节点，则将该定时器从链表中取出并重新插入链表
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        }
        //如果不是头结点，则插入其后的链表中
        else
        {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }

    //将目标定时器timer从链表中删除
    void del_timer(util_timer *timer)
    {
        if (!timer)//如果timer是空指针
        {
            return;
        }
        //下面这个条件成立表示链表中只有一个定时器即目标定时器
        if ((timer == head) && (timer == tail))
        {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        //链表中至少有两个定时器，目标定时器是链表头节点，重置为原头节点的下一个节点然后删除目标定时器
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        //链表中至少有两个定时器，且目标定时器是链表的尾结点，则将链表的尾结点重置为原尾结点的前一个节点，然后删除目标定时器
        if (timer == tail)
        {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        //如果目标定时器位于链表的中间，则把前后的定时器串联起来，然后删除目标定时器
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    //SIGALRM信号每次触发就在其信号处理函数中执行一次tick函数，以处理链表上到期的任务(如果使用统一事件源，则是主函数中执行tick函数)
    void tick()
    {
        if (!head)//如果head是空指针
        {
            return;
        }
        printf("timer tick\n");
        time_t cur = time(NULL);//获得系统当前的时间
        util_timer *tmp = head;
        //从头节点依次处理每个定时器，直到遇到一个尚未定期的定时器
        while (tmp)
        {
            //比较定时器是否到期
            if (cur < tmp->expire)
            {
                break;
            }
            //调用定时器的回调函数，以执行定时任务
            tmp->cb_func(tmp->user_data);
            //执行完定时器中的定时任务后，就将他从链表中删除，并重置链表头结点
            head = tmp->next;
            if (head)
            {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

private:
    //将目标定时器timer添加到节点lst_head之后的部分链表中
    void add_timer(util_timer *timer, util_timer *lst_head)
    {
        util_timer *prev = lst_head;
        util_timer *tmp = prev->next;
        //遍历lst_head节点之后的部分链表，直到找到一个超时时间大于目标定时器的超时时间的节点，并将目标定时器插入该节点之前
        while (tmp)
        {
            if (timer->expire < tmp->expire)
            {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }

        //遍历完lst_head节点后的部分链表，将目标定时器设为链表尾节点
        if (!tmp)//如果tmp是尾结点之后的节点
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

private:
    util_timer *head;//头节点
    util_timer *tail;//尾结点
};
#endif//LST_TIMER
