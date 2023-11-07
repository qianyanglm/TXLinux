#include <stdio.h>
#include <unistd.h>

int main()
{
    int x = 123;
    int y = 0123;
    int z = 0x123;
    printf("%d\n", x);
    printf("%d\n", y);
    printf("%d\n", z);
}