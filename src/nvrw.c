#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "nvmmio.h"
#include "nvrw.h"
#include "uma.h"
#include "debug.h"

#ifndef NULL
#define NULL 0
#endif

#define IO_MAP_SIZE (1UL << 32) /* 1GB */
#define FD_LIMIT 1024
#define PATH_SIZE 64
#define NthM(x) (67108864 << x) // 64M

//#define __OPEN_NEEDS_MODE(oflag) (((oflag) & O_CREAT) != 0)

/**
 * @brief 记录文件相关的数据
 * 
 */
typedef struct fd_mapaddr_struct {
  void *addr; // 记录映射起始地址
  off_t off;  // 文件内偏移
  char pathname[PATH_SIZE];
  size_t mapped_size; // 映射空间的大小，一般不变，用于unmap时的参数
  size_t written_file_size; // 映射文件的有效数据长度
  size_t current_file_size; // 文件在nvm上的大小
  int dup; // 记录复制的文件描述符次数
  int dupfd;  // 指示当前的fd是否是dup来的。如果fd_table[fd].dupfd != fd.则说明通过调用nvdup产生的fd。
  int open; // 文件被打开的次数，即打开同一文件产生的不同的文件描述符的个数（不包括dup）
  int increaseCount;  // 文件在nvm上空间扩展的次数，初始值为1
  uma_t *fd_uma;
} fd_addr;

/**
 * @brief 用于查找
 * 
 */
static fd_addr fd_table[FD_LIMIT] = {0,};
static int fd_indirection[FD_LIMIT] = {0};
static int lastFd;

int POSSIBLE_MODE = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP |
                    S_IROTH | S_IWOTH | S_IXOTH;

static inline void map_fd_addr(int fd, void *addr, off_t fd_size,
                               off_t written_file_size, size_t mapped_size,
                               const char *pathname) {
  fd_table[fd].addr = addr;
  fd_table[fd].off = 0;
  memcpy(fd_table[fd].pathname, pathname, strlen(pathname));
  fd_table[fd].mapped_size = mapped_size;
  fd_table[fd].written_file_size = written_file_size;
  fd_table[fd].current_file_size = fd_size;
  fd_table[fd].fd_uma = find_uma(addr);
  fd_table[fd].dupfd = fd;
  fd_table[fd].open = 0;
  fd_table[fd].dup = 0;
  fd_table[fd].increaseCount = 1;
  // TODO
  // getdtablesize() gives the MAX fd a process can have
  // not many fds, usually 1024.
  // Just make an array to keep a fd and addr pair
}

static inline off_t get_fd_off(int fd) {
  if (fd_table[fd].dupfd == fd)
    return fd_table[fd].off;
  else
    return fd_table[fd_indirection[fd]].off;
}
static inline void *get_fd_addr_cur(int fd) {
  return fd_table[fd_indirection[fd]].addr + get_fd_off(fd);
}

static inline void *get_fd_addr_set(int fd, off_t off) {
  return fd_table[fd_indirection[fd]].addr + off;
}

static inline uma_t *get_fd_uma(int fd) {
  uma_t *uma;

  LIBNVMMIO_INIT_TIME(get_fd_uma_time);
  LIBNVMMIO_START_TIME(get_fd_uma_t, get_fd_uma_time);

  uma = fd_table[fd_indirection[fd]].fd_uma;

  LIBNVMMIO_END_TIME(get_fd_uma_t, get_fd_uma_time);
  return uma;
}

static inline int get_path_fd(const char *pathname) {
  int i = 3;
  for (i = 3; i <= lastFd; i++) {
    if (fd_table[fd_indirection[i]].pathname != NULL &&
        fd_table[fd_indirection[i]].pathname != 0) {
      if (strcmp(fd_table[fd_indirection[i]].pathname, pathname) == 0) return i;
    }
  }
  return -1;
}

/**
 * @brief 修改对应的文件大小 
 * 使file_size = written_file_size
 */
static inline void trunc_fit_fd(int fd) {
	size_t written_file_size = fd_table[fd_indirection[fd]].written_file_size;
	size_t current_file_size = fd_table[fd_indirection[fd]].current_file_size;

	if (written_file_size < current_file_size) {
		if (ftruncate(fd, written_file_size) < 0) {
			LIBNVMMIO_DEBUG("ftruncate error");
		} else {
			fd_table[fd_indirection[fd]].current_file_size = written_file_size;
		}
	}
}

/**
 * @brief 拓展文件可占用空间大小
 * current_file_size <= 1GB  ->   1GB
 * current_file_size > 1GB   ->   +NthM(x)
 */
static inline size_t trunc_expand_fd(int fd, size_t current_file_size) {
  size_t ret = current_file_size;
  int indirectedFd = fd_indirection[fd];
  if (current_file_size < IO_MAP_SIZE) {
    if (posix_fallocate(indirectedFd, 0, IO_MAP_SIZE) < 0) { // 扩展磁盘空间，可以用于申请NVM上的空间？
      LIBNVMMIO_DEBUG("posix_fallocate error");
    } else {
      fd_table[indirectedFd].current_file_size = IO_MAP_SIZE; // 扩展成功
      ret = IO_MAP_SIZE;
    }
  } else {
    if (current_file_size == IO_MAP_SIZE) return IO_MAP_SIZE;

    unsigned long long add_file_size =
        NthM(fd_table[indirectedFd].increaseCount);
    ret += add_file_size;
    if (posix_fallocate(indirectedFd, fd_table[indirectedFd].current_file_size,
                        add_file_size) < 0) {
      LIBNVMMIO_DEBUG("posix_fallocate error");
    } else {  // 扩展成功
      fd_table[indirectedFd].increaseCount++;
      fd_table[indirectedFd].current_file_size = ret;
    }
  }
  return ret;
}

/**
 * @brief 扩展内存映射文件大小
 * 在unmap之前需要显示调用nvmsync
 * 然后重新建立映射
 */
static inline uma_t *expand_remap_fd(int fd, size_t current_file_size) {
  int indirectedFd = fd_indirection[fd];
  size_t ret = trunc_expand_fd(fd, current_file_size);

  LIBNVMMIO_DEBUG("addr:%ld, len:%ld",
								 (long int)fd_table[indirectedFd].addr,
								 fd_table[indirectedFd].written_file_size);

  /* sync */
  nvmsync(fd_table[indirectedFd].addr, fd_table[indirectedFd].written_file_size,
          MS_SYNC);

  nvmunmap_uma(fd_table[indirectedFd].addr, fd_table[indirectedFd].mapped_size,
               get_fd_uma(fd));
  
  /* 重新建立映射 */
  fd_table[indirectedFd].addr =
      nvmmap(NULL, ret, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  /* 修改fd_table中对应的数据 */
  if (fd_table[indirectedFd].addr) {
    fd_table[indirectedFd].mapped_size = ret;
    fd_table[indirectedFd].fd_uma = find_uma(fd_table[indirectedFd].addr);
  } else {
    LIBNVMMIO_DEBUG("Failed!!!");
  }
  return fd_table[indirectedFd].fd_uma;
}

/**
 * @brief 创建一个文件并且建立映射
 */
int nvcreat(const char *filename, mode_t mode) {
  /* TODO: should change this to open */
  int fd = creat(filename, mode);

  if (fd >= 0) {
    void *addr = nvmmap(NULL, IO_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (addr != MAP_FAILED) map_fd_addr(fd, addr, 0, 0, IO_MAP_SIZE, filename);
  }
  return fd;
}

/**
 * @brief 处理文件打开时的flags
 */
static inline void sanitize_flags(int *flags) {
  if (!(*flags & O_RDWR)) {
    if (!(*flags & O_WRONLY)) {
      if (!*flags & O_RDONLY) {
        LIBNVMMIO_DEBUG("open should include one of O_WRONLY, O_RDWR, O_RDONLY");
      }
      *flags ^= O_RDONLY;
      *flags |= O_RDWR;
    } else {
      *flags ^= O_WRONLY;
      *flags |= O_RDWR;
    }
  }
}

static inline int fd_validity(int fd) {
  return fcntl(fd, F_GETFL) != -1 || errno != EBADF;
}

/**
 * @brief 打开一个文件，并且建立映射，可以通过flags来设置是否选择普通打开
 * @param path 路径
 * @param flags 
 * @param ... 
 * @return int 
 */
int nvopen(const char *path, int flags, ...) {
  struct stat statbuf;
  off_t fd_size = 0;
  bool isdir = false;
  int fd, mode;

  /* TODO: Implement O_NONBLOCK and O_NODELAY */

  if (!(flags & O_ATOMIC)) {
		goto original_open;
  }
	else {
		flags &= ~O_ATOMIC;
	}

  if (flags & O_PATH) { // 解析路径名但不打开文件
		goto original_open;
  }

  if (stat(path, &statbuf) != 0) { // 获取文件元数据
    LIBNVMMIO_DEBUG("stat failed to Path:%s errno:%d", path, errno);
  } else {
    if (S_ISDIR(statbuf.st_mode) || strncmp(path, "/dev", 4) == 0) {
      isdir = 1;
    } else {
      fd_size = statbuf.st_size;
    }
  }

  if (isdir == 1 || strncmp(path, "/dev", 4) == 0 ||
      strncmp(path, "/proc", 5) == 0 ||
      (path[strlen(path) - 1] == '.' && path[strlen(path) - 2] == '/')) {
    isdir = true;
  } else { //如果是文件，处理flags
    sanitize_flags(&flags);
  }

  if (isdir == false || flags & O_CREAT) {
    va_list arg;
    int mode;

    va_start(arg, flags);
    mode = va_arg(arg, int);
    va_end(arg);

    if ((mode & POSSIBLE_MODE) == mode) {
      fd = open(path, flags, mode);
    } else {
      // This is when a file is opened with O_CREAT but with no mode specified
      // or wrong mode No exact definition on what happens when O_CREAT without
      // mode. NOVA seems like it cannot handle it. so giving 0666 as default
      // would be a good idea
      fd = open(path, flags, 0666);
    }
  } else {
    fd = open(path, flags);
    fd_table[fd].addr = NULL;
    fd_table[fd].dupfd = fd;
    fd_indirection[fd] = fd;

    if (lastFd < fd) lastFd = fd;
    return fd;
  }

  if (fd >= 0) {
    fd_table[fd].addr = NULL;
    off_t written_size = fd_size;
    size_t mapped_size;
    void *addr = 0;
    int openedFd = get_path_fd(path); // 寻找对应的一打开的fd

    struct timeval tv;
    gettimeofday(&tv, NULL);

    if (openedFd > 0) { // 找到对应的openedFd
      fd_table[fd].off = 0; // 初始化offset
      fd_table[fd].dupfd = fd;
      fd_indirection[fd] = openedFd; // 指向文件第一次被打开时的fd
    } else {
      fd_indirection[fd] = fd;
      openedFd = fd;
      mapped_size = trunc_expand_fd(fd, fd_size); // 扩展文件大小
      fd_size = mapped_size;
      addr = nvmmap(NULL, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (addr != MAP_FAILED)
        map_fd_addr(fd, addr, fd_size, written_size, mapped_size, path);
    }
    fd_table[openedFd].open++;
    if (lastFd < openedFd) lastFd = openedFd;
  } else {
    LIBNVMMIO_DEBUG("open failed for %s fd:%d errno:%d\n", path, fd, errno);
  }
  return fd;

original_open:
	mode = 0;

	if (__OPEN_NEEDS_MODE(flags)) {
		va_list arg;
		va_start(arg, flags);
		mode = va_arg(arg, int);
		va_end(arg);
	}
	return open(path, flags, mode);
}

/**
 * @brief 复制oldfd所指的文件描述符
 * 
 */
int nvdup(int oldfd) {
  int newfd = dup(oldfd);

  fd_indirection[newfd] = fd_indirection[oldfd];
  fd_table[newfd].dupfd = fd_table[oldfd].dupfd;
  fd_table[fd_table[oldfd].dupfd].dup++;

  return newfd;
}

/**
 * @brief 
 * 
 * @param vargp 
 * @return void* 
 */
//TODO:learn
void *unmap_thread(void *vargp) {
  int fd = *((int *)vargp);

  nvmsync(fd_table[fd].addr, fd_table[fd].written_file_size, MS_SYNC);
  nvmunmap_uma(fd_table[fd].addr, fd_table[fd].mapped_size, get_fd_uma(fd));
  fd_table[fd].addr = NULL;
	return NULL;
}

/**
 * @brief 
 */
int nvclose(int fd) {
  if (get_fd_addr_cur(fd) == NULL) {
    fd_indirection[fd] = 0;
    return close(fd);
  }

  if (fd_table[fd].dupfd != fd) { // 说明是dup而来
    fd_table[fd_table[fd].dupfd].dup--;
    if (fd_indirection[fd_table[fd].dupfd] == 0) {// dup的fd已经被关闭
      if (fd_table[fd_table[fd].dupfd].dup == 0 &&
          fd_indirection[fd_indirection[fd]] == 0) {
        if (fd_table[fd_indirection[fd]].open <= 2) {
          fd_table[fd].dupfd = 0;
          LIBNVMMIO_DEBUG("goto");
          goto removeOriginalFd;
        } else
          fd_table[fd_indirection[fd]].open--;
      }
    }
    fd_table[fd].dupfd = 0;
  } else if (fd_table[fd_indirection[fd]].open > 1) {
    if (fd_table[fd].dup == 0) {// 该fd未被复制
      fd_table[fd_indirection[fd]].open--;
      fd_table[fd].off = 0;
      fd_table[fd].dupfd = 0; // 标志该fd无效?
    }
  } else if (fd_table[fd].dup == 0) {// 该fd未被复制
    void *addr;
    //size_t mapped_size;
  removeOriginalFd:// 删除原始的fd
    addr = fd_table[fd_indirection[fd]].addr;
    //mapped_size = fd_table[fd_indirection[fd]].mapped_size;
    trunc_fit_fd(fd);
    close_sync_thread(fd_table[fd_indirection[fd]].fd_uma);// 关闭后台同步线程

    nvmsync_uma(addr, fd_table[fd_indirection[fd]].written_file_size, MS_SYNC,
                get_fd_uma(fd));
    // 取消文件映射
    nvmunmap_uma(fd_table[fd_indirection[fd]].addr,
                 fd_table[fd_indirection[fd]].mapped_size, get_fd_uma(fd));

    fd_table[fd_indirection[fd]].addr = NULL;
    fd_table[fd_indirection[fd]].off = 0;
    memset(fd_table[fd_indirection[fd]].pathname, 0, PATH_SIZE);
    fd_table[fd_indirection[fd]].mapped_size = 0;
    fd_table[fd_indirection[fd]].written_file_size = 0;
    fd_table[fd_indirection[fd]].current_file_size = 0;
    fd_table[fd_indirection[fd]].fd_uma = NULL;
    fd_table[fd_indirection[fd]].open = 0;
    fd_table[fd_indirection[fd]].dup = 0;
    fd_table[fd_indirection[fd]].dupfd = 0;
    fd_table[fd_indirection[fd]].increaseCount = 0;
  } else {
    fd_table[fd_indirection[fd]].open--;
  }
  fd_indirection[fd] = 0;

  return close(fd);
}

/**
 * @brief 向内存映射文件写入数据
 * 
 * @param fd 文件描述符
 * @param buf 待写入的数据
 * @param cnt 待写入的数据大小
 * @param dst 待写入的地址
 * @return ssize_t 
 */
static inline ssize_t pwriteToMap(int fd, const void *buf, size_t cnt,
                                  void *dst) {
  // void *dst = get_fd_addr_set(fd,off);
  // uma_t *dst_uma = find_uma(dst);
  uma_t *dst_uma = get_fd_uma(fd);

  /*
     if(fd_table[fd].addr == NULL){
  //printf("[%s]: Invalid write request from fd %d\n",__func__, fd);
  }
   */
  if (dst_uma) {
    // TODO Check if trunc_fit_fd is needed in libnvmmio mmap semantic
    // trunc_fit_fd(fd);
    unsigned long required_size =
        cnt + (dst - fd_table[fd_indirection[fd]].addr);
    if (required_size > fd_table[fd_indirection[fd]].current_file_size) {
      LIBNVMMIO_DEBUG("call expand remap fd current size:%ld required size:%lu",
                    fd_table[fd_indirection[fd]].current_file_size,
                    required_size);
      dst_uma = expand_remap_fd(fd, required_size);
      dst = get_fd_addr_cur(fd);  // required_size + fd_table[fd_indirection[fd]].addr;
    }
    /* write 次数加1 */
    increase_uma_write_cnt(dst_uma);

    nvmemcpy_write(dst, buf, cnt, dst_uma);
  } else {
    LIBNVMMIO_DEBUG("dst_uma for fd %d->%d  doesn't exist", fd, fd_indirection[fd]);
  }
  return cnt;
}

/**
 * @brief 读取src处的cnt字节的数据到buf中
 * 
 * @param fd 已打开文件的描述符
 * @param buf 待读取数据写入的缓冲区
 * @param cnt 字节数
 * @param src 待读取数据的地址
 * @return ssize_t 
 */
static inline ssize_t preadFromMap(int fd, void *buf, size_t cnt, void *src) {
  uma_t *src_uma = get_fd_uma(fd);

  /*
     if(fd_table[fd_indirection[fd]].addr == NULL){
  //printf("[%s]: Invalid read request from fd %d\n",__func__, fd);
  }
   */

  if (src_uma) {
    increase_uma_read_cnt(src_uma);
    if (src_uma->policy == UNDO) {
      nvmmio_memcpy(buf, src, cnt); // undo策略由于是就地写，可以直接读取
      return cnt;
    } else {
      nvmemcpy_read_redo(buf, src, cnt);// 从redo log中读取
    }
  }
  return cnt;
}

/**
 * @brief 从已打开的文件中读取cnt字节的数据到buffer中，通过调用preadFromMap
 * 
 * @param fd 打开的文件描述符
 * @param buf  读取数据所存放的缓冲区
 * @param cnt 读取数据的size
 * @return ssize_t 
 */
ssize_t nvread(int fd, void *buf, size_t cnt) {
  void *src = get_fd_addr_cur(fd);
  if (src == NULL) {
    //	printf("[%s] Called write with unmapped fd %d\\n", __func__, fd);
    return read(fd, buf, cnt);
  }
  ssize_t ret = preadFromMap(fd, buf, cnt, src);
  if (fd_table[fd].dupfd == fd) { // 不是被复制的fd
    fd_table[fd].off += cnt;
  } else {
    // TODO: 编码试试dup和reopen的差别
    fd_table[fd_indirection[fd]].off += cnt;// CONFUSE：为什么不是fd_table[fd_table[fd].dupfd].off += cnt?
  }

  return ret;
}

/**
 * @brief 将buf缓冲区中的cnt字节的数据写入到对应文件中
 * 
 * @param fd 文件描述符
 * @param buf 待写入的数据
 * @param cnt 待写入的数据长度
 * @return ssize_t 
 */
ssize_t nvwrite(int fd, const void *buf, size_t cnt) {
  void *dst;

  dst = get_fd_addr_cur(fd);

  if (dst == NULL) {
    // printf("[%s] Called write with unmapped fd %d\\n", __func__, fd);
    return write(fd, buf, cnt);
  }
  ssize_t ret = pwriteToMap(fd, buf, cnt, dst);

  // printf("\t\t\t[%s]: write Length:%ld fd:%d\n", __func__, cnt, fd);
  off_t off;
  if (fd_table[fd].dupfd == fd) {
    fd_table[fd].off += cnt;
    off = fd_table[fd].off;
  } else {
    fd_table[fd_indirection[fd]].off += cnt;
    off = fd_table[fd_indirection[fd]].off;
  }
  if ((size_t)off > fd_table[fd_indirection[fd]].written_file_size) {
    fd_table[fd_indirection[fd]].written_file_size = (size_t)off;
  }

  return ret;
}

off_t nvlseek(int fd, off_t offset, int whence) {
  off_t off;
  switch (whence) {
    case SEEK_SET:// 参数offset 即为新的读写位置
      // validate offset range
      if (fd_table[fd].dupfd == fd)
        fd_table[fd].off = offset;
      else
        fd_table[fd_indirection[fd]].off = offset;

      return offset;

    case SEEK_CUR:// SEEK_CUR 以目前的读写位置往后增加offset 个位移量
      if (fd_indirection[fd] == 0) return -1;
      if (fd_table[fd].dupfd == fd) {
        fd_table[fd].off += offset;
        off = fd_table[fd].off;
      } else {
        fd_table[fd_indirection[fd]].off += offset;
        off = fd_table[fd_indirection[fd]].off;
      }
      return off;

    case SEEK_END:  // 将读写位置指向文件尾后再增加offset 个位移量.
      if (fd_table[fd].dupfd == fd) {
        fd_table[fd].off = fd_table[fd].written_file_size + offset;
        off = fd_table[fd].off;
      } else {
        fd_table[fd_indirection[fd]].off =
            fd_table[fd].written_file_size + offset;
        off = fd_table[fd_indirection[fd]].off;
      }
      return off;

    default:
      // SEEK_DATA, SEEK_HOLE if needed
      return EINVAL;
  }
}

/**
 * @brief 修改文件大小
 */
int nvftruncate(int fd, off_t length) {
  int ret = ftruncate(fd, length);
  if (ret == 0) {
    fd_table[fd_indirection[fd]].written_file_size = length;
    fd_table[fd_indirection[fd]].current_file_size = length;
    // TODO check sparse file is posix standard
  }

  return ret;
}

/**
 * @brief 同步，对于ummaped文件，调用fsync，否则调用nvmsync_uma
 */
int nvfsync(int fd) {
  int indirectedFd = fd_indirection[fd];
  if (get_fd_addr_cur(fd) == NULL) {
    return fsync(fd);
  }
  // trunc_fit_fd(fd);
  // printf("[%s]:addr:%ld, len:%ld\n",__func__,(long int)fd_table[fd].addr,
  // fd_table[fd].written_file_size);
  return nvmsync_uma(fd_table[indirectedFd].addr,
                     fd_table[indirectedFd].written_file_size, MS_ASYNC,
                     get_fd_uma(fd));
}

// pread does not change offset
/**
 * @brief  read并指定offset
 */
ssize_t nvpread(int fd, void *buf, size_t cnt, off_t offset) {
  if (get_fd_addr_cur(fd) == NULL) {
    //	printf("[%s] Called write with unmapped fd %d\\n", __func__, fd);
    return pread(fd, buf, cnt, offset);
  }
  void *src = get_fd_addr_set(fd, offset);
  ssize_t ret = preadFromMap(fd, buf, cnt, src);

  return ret;
}
ssize_t nvpread64(int fd, void *buf, size_t cnt, off_t offset) {
  return nvpread(fd, buf, cnt, offset);
}

/**
 * @brief write但不更新offset
 */
ssize_t nvpwrite(int fd, const void *buf, size_t cnt, off_t offset) {
  if (get_fd_addr_cur(fd) == NULL) {
    return pwrite(fd, buf, cnt, offset);
  }
  ssize_t ret = pwriteToMap(fd, buf, cnt, get_fd_addr_set(fd, offset));

  off_t written_size = offset + cnt;
  if ((size_t)written_size > fd_table[fd_indirection[fd]].written_file_size) {
    fd_table[fd_indirection[fd]].written_file_size = written_size;
  }

  return ret;
}
ssize_t nvpwrite64(int fd, const void *buf, size_t cnt, off_t offset) {
  return nvpwrite(fd, buf, cnt, offset);
}

// TODO Implement this as multithreaded from thread pool made in init()
/**
 * @brief 读取数据到多个buffer，并不是并行
 */
ssize_t nvpreadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
  // TODO Stopped Here
  int i;
  ssize_t ret = 0;
  if (get_fd_addr_cur(fd) == NULL) {
    return preadv(fd_indirection[fd], iov, iovcnt, offset);
  }
  void *src = get_fd_addr_set(fd, offset);

  for (i = 0; i < iovcnt; i++) {
    ret += preadFromMap(fd, iov[i].iov_base, iov[i].iov_len, src);
    src += iov[i].iov_len;
  }

  return ret;
}

/**
 * @brief 写入多个buffer的数据
 */
ssize_t nvpwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
  if (get_fd_addr_cur(fd) == NULL) {
    return pwritev(fd, iov, iovcnt, offset);
  }
  int i;
  ssize_t ret = 0, file_size = fd_table[fd_indirection[fd]].current_file_size,
          len = offset;
  void *dst = get_fd_addr_set(fd, offset);

  for (i = 0; i < iovcnt; i++) {
    len += iov[i].iov_len;
    if (len > file_size) {
      expand_remap_fd(fd, file_size);
    }
    ret += pwriteToMap(fd, iov[i].iov_base, iov[i].iov_len, dst);
    dst += iov[i].iov_len;
  }

  off_t written_size = offset + ret;
  if (written_size > file_size) {
    fd_table[fd_indirection[fd]].written_file_size = written_size;
  }

  return ret;
}

/**
 * @brief 读取数据到多个buffer，并更新offset
 */
ssize_t nvreadv(int fd, const struct iovec *iov, int iovcnt) {
  int i;
  ssize_t ret = 0;
  void *src = get_fd_addr_cur(fd);
  if (src == NULL) {
    //	printf("[%s] Called write with unmapped fd %d\\n", __func__, fd);
    return readv(fd, iov, iovcnt);
  }

  for (i = 0; i < iovcnt; i++) {
    ret += preadFromMap(fd, iov[i].iov_base, iov[i].iov_len, src);
    src += iov[i].iov_len;
  }

  if (fd_table[fd].dupfd == fd)
    fd_table[fd].off += ret;
  else
    fd_table[fd_indirection[fd]].off += ret;

  return ret;
}

/**
 * @brief 写入多个buffer中的数据，并更新offset
 */
ssize_t nvwritev(int fd, const struct iovec *iov, int iovcnt) {
  int i;
  ssize_t ret = 0, file_size = fd_table[fd_indirection[fd]].current_file_size,
          len;
  void *dst = get_fd_addr_cur(fd);
  if (dst == NULL) {
    return writev(fd, iov, iovcnt);
  }

  if (fd_table[fd].dupfd == fd)
    len = fd_table[fd].off;
  else
    len = fd_table[fd_indirection[fd]].off;

  for (i = 0; i < iovcnt; i++) {
    ssize_t iovlen = iov[i].iov_len;
    len += iovlen;
    if (len > file_size) {
      expand_remap_fd(fd, file_size);
    }
    ret += pwriteToMap(fd, iov[i].iov_base, iovlen, dst);
    dst += iovlen;
  }

  if (fd_table[fd].dupfd == fd) {
    fd_table[fd].off += ret;
    if (fd_table[fd].off > file_size)
      fd_table[fd_indirection[fd]].written_file_size = fd_table[fd].off;
  } else {
    fd_table[fd_indirection[fd]].off += ret;
    if (fd_table[fd_indirection[fd]].off > file_size)
      fd_table[fd_indirection[fd]].written_file_size =
          fd_table[fd_indirection[fd]].off;
  }

  return ret;
}

/**
 * @brief  fdatasync函数类似于fsync，但它只影响文件的数据部分
 */
int nvfdatasync(int fd) {
  if (get_fd_addr_cur(fd) == NULL) {
    // printf("\n\n%s called ", __func__);
    int ret = fdatasync(fd);
    // printf("ret : %d ", ret);
    if (ret < 0)
      // printf(" errno:%d\n", errno);
      return ret;
  }
  LIBNVMMIO_DEBUG("fd:%d", fd);
  return nvfsync(fd);
}

/**
 * @brief  
 */
int nvfcntl(int fd, int cmd, ...) {
  va_list arg;
  struct flock *f1;
  int flags;
  switch (cmd) {
    case F_SETLK:
      va_start(arg, cmd);
      f1 = va_arg(arg, struct flock *);
      va_end(arg);
      return fcntl(fd_indirection[fd], cmd, f1);
    case F_SETFD:
      va_start(arg, cmd);
      flags = va_arg(arg, int);
      va_end(arg);
      sanitize_flags(&flags);
      return fcntl(fd_indirection[fd], cmd, flags);
    case F_GETFD:
      // return fd flags
      return fcntl(fd_indirection[fd], cmd);
    default:
      // printf("[%s]: the cmd:%d is not defined in %s\n", __func__, cmd,
      // __func__);
      return 0;
  }
}


/**
 * @brief 获取文件状态
 */
int nvstat(const char *pathname, struct stat *statbuf) {
  int ret = stat(pathname, statbuf);
  int fd = get_path_fd(pathname);
  statbuf->st_size = fd_table[fd_indirection[fd]].written_file_size;
  return ret;
}

int nvunlink(const char *pathname) {
  int fd = get_path_fd(pathname);
  if (fd > 0) {
    nvclose(fd);
  }

  return unlink(pathname);
}

int nvrename(const char *oldpath, const char *newpath) {
  int fd = get_path_fd(oldpath);
  if (fd > 0) {
    memcpy(fd_table[fd_indirection[fd]].pathname, newpath, strlen(newpath));
  }
  return rename(oldpath, newpath);
}

int nvposix_fadvise(int fd, off_t offset, off_t len, int advice) {
  return posix_fadvise(fd_indirection[fd], offset, len, advice);
}

int nvfstat(int fd, struct stat *statbuf) {
  int ret = fstat(fd, statbuf);
  statbuf->st_size = fd_table[fd_indirection[fd]].written_file_size;
  return ret;
}

/**
 * @brief sync指定位置的指定大小的数据
 */
int nvsync_file_range(int fd, off64_t offset, off64_t nbytes) {
  trunc_fit_fd(fd);
  LIBNVMMIO_DEBUG("addr:%ld, len:%ld", (long int)fd_table[fd].addr, fd_table[fd].written_file_size);
  return nvmsync(fd_table[fd_indirection[fd]].addr + offset, nbytes, MS_ASYNC);
}

/**
 * @brief 从fd中offset处开始，保证文件长度不小于len。
 */
int nvfallocate(int fd, int mode, off_t offset, off_t len) {
  // TODO: zero out unwritten area to end of file
  off_t required_len = offset + len;
  size_t current_file_size = fd_table[fd_indirection[fd]].current_file_size;
  LIBNVMMIO_DEBUG("");

  if (current_file_size < (size_t)required_len) {
    int ret = fallocate(fd, mode, offset, len);
    if (ret < 0)
      return ret;
    else {
      fd_table[fd_indirection[fd]].current_file_size = required_len;
    }
    return ret;
  } else {
    return fallocate(fd, mode, offset, len);
  }
}
