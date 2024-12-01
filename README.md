# Single-File File System

A single file stores the entire file system.
You mount the file system by specifying the file name.



## NUMBERS

1 Byte = 8 bits
1 KB = 1024 Bytes = 2^10 Bytes
1 MB = 1024 KB = 2^20 Bytes
1 GB = 1024 MB = 2^30 Bytes
1 TB = 1024 GB = 2^40 Bytes

unsighed int = 4 bytes, covers upto 2^32 blocks
Block numbers will be unsigned int. Max 2^32 blocks.

1 Block = 1 KB = 2^10 Bytes
Max system size = 2^32 * 2^10 = 2^42 Bytes = 4 TB
1 Pointer block has upto 2^10/4 = 256 block numbers
Coverage = 256 * 2^10 = 2^18 = 256 KB


1 Block = 4 KB = 2^12 Bytes
Max system size = 2^32 * 2^12 = 2^44 Bytes = 16 TB
1 Pointer block has upto 2^12/4 = 1024 block numbers
Coverage = 1024 * 2^12 = 2^22 = 4 MB



M1 macs have 16KB page size
X86-64 has 4KB page size



## Strategy
Every read/write operation will translate to some read/write operations on the file.

Each block is of fixed size.
Each block is identified by a block number.

METADATA BLOCK
- This is the first block
- Has the root directory block number
- Size of the directory block

POINTER BLOCK
- Has block number of the next block
- Rest is full of block numbers

FILE BLOCK
    - Contains binary data

DIRECTORY BLOCK
    - Contains a list of (file name, block number xk, pointer block, size)
    - Contains a list of (directory name, block number xk, pointer block, size)

Memory
- First build the tree and store it in memory.
- Build the free list, which is (start,end) of empty sequence of blocks.
- Cache last K data blocks. Check for OOM pressure and keep writing blocks to the file. You don't want swaps to happen.


## Operations
Open: Set the FD, tie it to the file name
Read: Translate the offset to block number, read the block (if in cache)
Write: Translate the offset to block number, cache the updated block. Borrw fresh block number if needed.
Flush: Write the cache to the file. Update metadata.


## Brainstorm
- Mac/UNIX will do prefetching of data pages. Therefore it makes sense to keep the data blocks a file next to each other.
- Also means that there is no reason for me to cache blocks that I expect OS to cache.


## Measurements
Percentage of the blocks that are unused
