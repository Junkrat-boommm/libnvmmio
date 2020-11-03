# 背景
传统的文件系统限制了NVM的性能（software overhead）

![avatar](./photo/1.png)

# 目的
充分发挥NVM的高性能

# 存在的问题
1. 基于kernel的数据访问具有较高的延迟，**mmap io**提供直接NVM访问，能够有效减低kernel开销。

    > 减少了用户空间和page cache之间的数据交换  

2. mmap io不提供写数据的原子性，并且为了保证crash-safe，cache line应刷新以确保持久性，并应使用*内存隔离*以为NVM更新提供正确的持久顺序，这往往会带来大量开销，并且难以编程。

3. 现有的一致性保证机制中CoW存在写放大以及TLB-shootdown问题，journaling（logging）的两种方式有不同的适应场景。
    - redo log  
    先将数据写入redo log，再将log持久化到目标文件。redo log中记录最新的数据。（适合写）
    - undo log  
    先复制目标文件中的数据到undo log中，再对目标文件进行就地更新。目标文件中记录最新的数据。（适合读）  
    
    对于可按字节读写的NVM设备，混合日志可显著减少写放大。

# libnvmmio
## 设计目标和实现策略
* 低延时：避免使用内核IO路径。
* 原子性：使用日志维护数据操作原子性
* 高吞吐、高并发：灵活的数据结构、varying sizes and fine-grained logging。
* 以数据为中心，per-block的组织方式：基于inode的log对于同一文件的并发访问不友好。
* 对底层文件系统透明

## Overall Architecture
**memory-mapped IO**
为了直接访问NVM。libnvmmio通过mmap建立文件映射，应分别用memcpy和non-temporal memcpy(MOVNT)来代替read和write方法。有如下两个好处。
* 当持久化和读取数据时，能够避免复杂的内核IO路径
* read/write操作涉及复杂的索引操作来定位物理块。而通过mmap io在建立映射后通过内存映射地址和偏移即可访问文件数据。而且也并不需要通过MMU和TLB完成到物理空间的映射，减少了大量的CPU开销。

**Scalable Logging**
