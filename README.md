# Single-File File System

A single file stores the entire file system.
You mount the file system by specifying the file name.

## Description
A single-file file system is a Virtual File System (Fuse) that is backed by a single file. 
It must support all the operations you expect from a file system while making an effort to provide the following properties:
- Low Latency operations (Sequential and Random reads/writes)
- High throughput operations
- Crash consistency/recovery.
- Efficient use of space (minimal overhead)

Nongoals that an alternative file system might provide:
- Encryption
- Compression
- Concurrent access

Inspiration is QCOV2, a single file that stores the entire state of the Virtual Machine and can be mounted.

Two approaches to build such a file system are:
- Compress/Decompress the file system when mounting/unmounting. Every read/write goes to the decompressed tmp/files, which are then written back when you're done with all your operations / occasionally.
- Lay-out your file system in a way that you can directly read/write to the file without needing to decompress it.

In particular, I want to preserve the properties of a regular file system so any program can use it without caring about the difference. 

## Design
The file is broken down into fixed-size blocks. These effectively form a log-structure, where new files are added on to blocks at the end, or are interleaved in holes left by deleted files.

Directory Block
- Contains a list of descriptors that contain basic metadata and point to the object's data blocks.
- This is a self-contained block and can be read by itself.
- It contains a pointer to the next directory block if the number of entries exceeds the block size. So you're generally interested in tracking the first one in the list.

```cpp
struct descriptor {
    uint32_t type; // 0 = file, 1 = directory
    char name[256];
    long size;
    uint32_t blocks[8];
    uint32_t pointer_block; // Valid if > 0
};


struct directory_block {
    uint32_t num_entries; // In this block
    uint32_t next_block; // Valid if > 0
    descriptor descriptors[(BLOCK_SIZE - 8) / sizeof(descriptor)];
};
```

Data Block
- Contains the binary data of the file.
- Cannot be read by itself. You need to have parsed the tree first.
- You need to know the size of the file to know how many blocks to read.

```cpp
struct data_block {
    char data[BLOCK_SIZE];
};
```

Pointer Block
- Contains block numbers of other blocks. These can be read to build the tree.
- The first two block numbers are reserved for the next pointer block and the number of pointers in this block.
```cpp
struct pointer_block {
    uint32_t next_pointer_block; // Valid if > 0
    uint32_t num_pointers; // In this block
    uint32_t blocks[BLOCK_NUMBERS_PER_BLOCK-2];
};
```



Free List
- Any time a file is deleted or truncated, the blocks are added to the free list.
- The free list is a sequence of block ranges that are free to be used.
- The free list is maintained in memory and is not persisted. It is rebuilt on mount.
- Size of free list dictates the wasted space in the file system.



File System Mechanics:
- Every read/write operation will translate to some read/write operations on the file.
- The entire tree is built in memory and preserved. Any change is persisted to the file.
- We do not cache any data blocks since we expect the OS to cache them.
- The main overhead comes from having to repeatedly call fsync to ensure the changes are persisted to the file. This has to happen at the scale of the whole file system, not individual files.
- Another overhead is in having to build the tree in memory, and the space it takes up.



## Evaluation

### Experiment 1
Sequentially write 100 files of 100 MB each.
Measure the time taken to write the files.
Expectation: Should take same time as writing to as the native file system.

Sequentially read 100 files of 100 MB each.
Measure the time taken to read the files.
Expectation: Should take same time as reading from the native file system.


### Experiment 2
Build a large file system (16GB).
16KB blocks --> 10^6 blocks
Measure the time taken to mount the file system.



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
