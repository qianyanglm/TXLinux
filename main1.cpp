#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void sig_handler(int num)
{
    printf("receive the signal %d.\n", num);
    alarm(2);
}

int main()
{
    signal(SIGALRM, sig_handler);
    alarm(2);
    while (1)//做一个死循环，防止主线程提早退出，相等于线程中的join
    {
        pause();
    }
    //pause();//如果没有做一个死循环则只会让出一次cpu然后就还给主线程，主线程一旦运行结束就会退出程序
    exit(0);
}