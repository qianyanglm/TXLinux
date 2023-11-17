#include <stdio.h>

int main()
{
    int a[5] = {1, 2, 3, 4, 5};
    int *ptr = (int *) (&a + 1);
    printf("%d %d\n", *(a + 1), *(ptr - 1));

    printf("a = %p\n", a);
    printf("&a = %p\n", &a);
    printf("&a[0] = %p\n", &a[0]);
    printf("&a[4] = %p\n", &a[4]);
    printf("ptr = %p\n", ptr);
    printf("&a+1 = %p\n", &a + 1);
    printf("*ptr = %d\n", *ptr);
    printf("ptr-1 = %p\n", ptr - 1);

    return 0;
}
