#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

#define PGSIZE 4096

void
task3(){
    char *arr[20];
    if (fork() == 0) {
        for (int i = 0; i < 5; ++i) {
            arr[i] = sbrk(PGSIZE);
            arr[i][0] = 'x';
            printf("arr[%d] va: %p got accessed\n",i,arr[i]);
        }
        for (int i = 5; i < 13; ++i) {
            arr[i] = sbrk(PGSIZE);
        }

        for (int i = 13; i < 20; ++i) {
            arr[i] = sbrk(PGSIZE);
        }
        if (fork() == 0) {
            sleep(1);
            arr[0][0]='a';
            exit(0);
        } else {
            arr[1][0]='b';
            wait(0);
        }
    } else {
            wait(0);
    }
}
int
main(int argc, char *argv[])
{
    task3();
    exit(0);
}
