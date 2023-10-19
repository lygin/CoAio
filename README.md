## build
```
mkdir build
cd build
cmake ..
make
```
## run
./coro_test

## usage
```c++
fd = open(xxx)
// simple
AioTask t;
WaitGroup wg;
wg.Add(1);

t.op = WRITE;
t.fd = fd;
t.buf = buffer_ptr;
t.off = file_offset;
t.len = write_len;
t.wg = &wg

sche->submit_aio(&t);

// batch
AioTask *batch[batch_cnt];
...
sche->submit_aio_batch(batch, batch_cnt);

```

## example
coro_test.cc
