#include <argz.h>
#include <cstdio>
#include <cstdlib>
#include <event.h>
#include <sys/signal.h>

int main(int argc, char *argv[])
{
    printf("%s", basename(argv[0]));

    return 0;
}