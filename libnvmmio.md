# 背景
传统的文件系统限制了NVM的性能（software overhead）


# 目的
充分发挥NVM的高性能

# 存在的问题
1. 基于kernel的数据访问具有较高的延迟，**mmap io**提供直接NVM访问，能够有效减低kernel开销。

    > 减少了用户空间和page cache之间的数据交换  

2. mmap io不提供写数据的原子性，并且为了保证crash-safe，cache line应刷新以确保持久性，并应使用*内存隔离*以为NVM更新提供正确的持久顺序，这往往会带来大量开销，并且难以编程。