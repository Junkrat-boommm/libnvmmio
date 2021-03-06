# Notice
We are updating this repository to improve the readability of our source code.
We will also add some examples that use Libnvmmio to help you understand how to use Libnvmmio.

# Libnvmmio
We have designed and implemented *Libnvmmio* to maximize the IO performance of non-volatile main memory (NVMM) systems. 
The purpose of Libnvmmio is eliminating software overhead, providing low-latency, scalable file IO while ensuring data-atomicity.
As the name indicates, Libnvmmio is linked with applications as a library, providing an efficient IO path using the ```mmap``` interface. 


## Requirements
1. **NVM-aware filesystem.**
You can use Libnvmmio with any filesystem that provides ```DAX-mmap```, such as Ext4-DAX, XFS-DAX, [PMFS](https://github.com/linux-pmfs/pmfs), [NOVA](https://github.com/NVSL/linux-nova), [SplitFS](https://github.com/utsaslab/SplitFS) and so on.
The ```DAX-mmap``` allows Libnvmmio to map the pages of an NVMM-backed file into its address space and then access it via ```load``` and ```store``` instructions.
Libnvmmio intercepts and replaces ```read()```/```write()``` system calls with ```load```/```store``` instructions. 

2. **PMDK.**
Libnvmmio uses [PMDK](https://pmem.io/pmdk/) library to write data to NVM.
When writing data through non-temporal stores, it uses ```pmem_memcpy_nodrain()``` and ```pmem_drain()```.
It also uses ```pmem_flush()``` to make metadata updates permanent.
PMDK is a well-proven library.
It provides optimizations for parallelisms such as SSE2 and MMX.
It will also support ARM processors as well as Intel processors.

## Getting started with Libnvmmio
1. **Build Libnvmmio.**
   ```
   $ git clone https://github.com/review-anonymous/libnvmmio.git
   $ cd libnvmmio/src
   $ make
   ```
2. **Write and compile your application.**
There are simple examples in the [examples directory](examples). Those examples will help you understand how to use Libnvmmio.
Note the following:
   * Include ```libnvmmio.h``` header file in the source file with the file IO code.
   * Use the ```O_ATOMIC``` flag when opening a file for which you want to guarantee atomic-durability.

3. **Set ```PMEM_PATH``` variable.**
Libnvmmio has to know where the persistent area is for logging.
So you have to tell Libnvmmio which path an nvm-aware filesystem is mounted on.
   ```
   $ export PMEM_PATH=/mnt/pmem
   ```
   
4. **Run your application.**
