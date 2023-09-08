#include "coro.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include "timer.h"
const int N = 4096;
const int BATCH_SIZE = 10'0000;

int main()
{
    Corot::Sche *sche = new Corot::Sche(8);
    int fd = open("testfile.txt", O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0)
    {
        perror("open");
    }
    // prepare data
    char *buf[BATCH_SIZE] = {0};
    for (int i = 0; i < BATCH_SIZE; i++)
    {
        posix_memalign((void **)&buf[i], 512, N);
        memset(buf[i], 0, N);
        for (int j = 0; j < N; j++)
            buf[i][j] = 'a' + i;
    }
    WaitGroup wg;
    AioTask t[BATCH_SIZE];
    wg.Add(BATCH_SIZE);
    
    
    // run
    Timer tm;
    for (int i = 0; i < BATCH_SIZE; i++)
    {
        t[i].op = WRITE;
        t[i].fd = fd;
        t[i].buf = buf[i];
        t[i].off = i * N;
        t[i].len = N;
        t[i].wg = &wg;
        sche->submit_aio(&t[i]);
    }
    wg.Wait();
    double elapsed = tm.GetDurationMs();
    printf("insert IOPS: %.2fKops\n avg latency: %.2fus\n", BATCH_SIZE * 1.0 / elapsed, elapsed * 1024 / BATCH_SIZE);

    int batch_cnt = 128;
    AioTask *batch[batch_cnt];
    WaitGroup wg2;
    wg2.Add(batch_cnt);
    for(int i=0; i<batch_cnt; ++i) {
        t[i].op = WRITE;
        t[i].fd = fd;
        t[i].buf = buf[i];
        t[i].off = (BATCH_SIZE+i) * N;
        t[i].len = N;
        t[i].wg = &wg2;
        batch[i] = &t[i];
    }
    tm.Reset();
    sche->submit_aio_batch(batch, batch_cnt);
    wg2.Wait();
    elapsed = tm.GetDurationMs();
    printf("insert IOPS: %.2fKops\n avg latency: %.2fus\n", batch_cnt * 1.0 / elapsed, elapsed * 1024 / batch_cnt);

    coro_run(sche, [&fd]{
        char *bufr;
        posix_memalign((void **)&bufr, 512, N);
        int ret = read(fd, bufr, N);
        printf("read: %d %s\n", ret, bufr);
    });
    return 0;
}