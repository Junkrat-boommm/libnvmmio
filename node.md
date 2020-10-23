# 结构布局：



# 函数设计：
**nvmsync(void *addr, size_t len, int flags)** 将内存映射文件写回
   1. 根据addr查找uma。（先cache在rbtree）
   2. nvmsync_uma持久化uma。

**nvmsync_uma(void *addr, size_t len, int flags, uma_t *uma)**持久化uma，并调用nvmsync_sync持久化文件
   1. 对uma上锁。
   2. 全局epoch加1
   3. flush uma. **nvmmio_flush**
   4. 更新log policy
   5. 调用nvmsync_sync持久化文件
   
**nvmsync_sync(void *addr, size_t len, unsigned long new_epoch)**将内存映射文件同步回PM
   1. 根据address映射table。 **radix tree的组织方式**
   2. 根据len获取到每一个可能需要sync的entry所在的table。
   3. 对每一个epoch < new_epoch的entry进行同步并flush
   4. 释放index_entry回local list中
   5. 释放local list回global list



**nvwrite(fd, buf, cnt):** 写操作的入口
   1. get_fd_addr_cur()获得dst
   2. pwriteToMap(fd, buf, cnt, dst)
      1. 获取uma信息
      2. 若空间不足，则进行重映射并expand。
         1. 扩展PM空间
         2. **SYNC**
         3. munmap uma：从rbtree中删除该uma的信息
         4. nvmmap
         5. 修改fd_table中的信息并返回uma
      3. 写请求次数加一
      4. **nvmemcpy_write**
   3. 更新静态数组fd_table中对应的信息，包括offset、written_file_size等。

**nvmemcpy_write(dst, buf, cnt, dst_uma)：** 将数据写入到log entry中
   1. 对uma上锁
   2. 获取Index对应的Table（从global_table_list中获取）
   3. 根据写入内容的大小设置log_size(log_entry的大小)
   4. 获得entry在table中的索引
     *循环体*将数据写入多个连续的entry（有可能跨table）
      1. 从table中索引对应的index entry（若为NULL，从global_entry_list中申请，其中包括对log entry的空间申请，从global_data_list中获取）
      2. 对index entry上锁
      3. 若是已经commit，则**sync_entry** ```什么情况下会出现这种情况？```
      4. 根据log policy执行写入操作 nvmmio_write
      5. 处理overwrite。这一部分的逻辑主要是将pre_log和刚写的log中间的空白进行填充，以保证entry中数据的连续性，并能够通过offset和len记录有效数据。
      6. 记录entry中的offset、len和dst。（``
      7. `通过dst和offset可以获得写回的位置吗？```）
      8. 持久化index entry（```这个操作是否有必要？```）
   5. 如果是undo log，则就地更新。

**sync_entry(log_entry_t *entry, uma_t *uma)** 对已提交的log_entry进行写回（需上锁）。
    1. 根据entry中的dst和offset得出dst，如果是redo log则进行写回，并且更新结构体数据。
   
