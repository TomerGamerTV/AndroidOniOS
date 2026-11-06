// What bionic reports for the main thread's stack (it parses /proc/self/maps for it).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    pthread_attr_t a;
    int rc = pthread_getattr_np(pthread_self(), &a);
    void *sp; size_t sz;
    pthread_attr_getstack(&a, &sp, &sz);
    int local;
    printf("getattr rc=%d stack %p size %zu, a local is at %p\n", rc, sp, sz, (void *)&local);
    return 0;
}
