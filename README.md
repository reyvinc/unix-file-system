# Unix-like File System

This is a unix-like file system implementation in C++. It supports operations such as mounting the disk, creating files and directories, writing to and reading from files, deleting files and directories, moving through directories and subdirectories, and defragmentation. 

# Supported Commands

`M <disk>` - mounts a disk to the file system
`C <file_name> <file_size` - creates a file with the specified name and size
`E <file_name> <file_size>` - resizes a file from the old size to the new size
`D <file_name>` - deletes a file/ subdirectory (recursively) if it exists in the current directory
`R <file_name> <n>` - reads the nth block of the specified file into the buffer
`W <file_name> <n>` - writes the buffer to the nth block of the specified file
`B <characters>` - flushes the buffer and updates it with the new characters
`L` - recursively lists files and subdirectories inside the current directory, showing file sizes for files and number of files for directories 
`O` - defrags the disk
`Y` - switch current directory 

## File System Design
Disks are assumed to be a 128KB file, consisting of 128 blocks (1KB each).

- The first block is the `super_block`, containing information about the file system.
- The first 128 bits (= 16 bytes) in the `super_block` represent the usage of each of the 128 blocks, whether they're currently in use or not.
- The next 1008 bytes represent 126 `Inode`s. Each `Inode` consumes 8 bytes and represents a file or a directory, and are explained in the following section:

## Inode
The 8 bytes are split as follows:
- The first 5 bytes are for the `name` of the file/ directory 
- The 6th byte is the `used_size`, how much the `Inode` is currently taking
- The 7th byte is `start_block`, which is the index of the start_block for the Inode
- The 8th byte is `dir_parent`, which is the index in the `super_block` to the file or directory's parent

## Implemented Methods
- `void fs_mount(char *name)`
Mounts the file system residing on the virtual disk with the specified name. The mounting process involves loading the superblock of the file system, but before doing this, you should check if there exists a file (i.e., a virtual disk) with the given name in the current working directory.

- `void fs_create(char name[5], int size) `
Creates a new file or directory in the current working directory with the given name and the given number of blocks, and stores the attributes in the first available inode. A size of zero means that the user is creating a directory.

- ` void fs_delete(char name[5])`
Deletes the file or directory with the given name in the current working directory. If the name represents a directory, your program should recursively delete all files and directories within this directory. For every file or directory that is deleted, you must zero out the corresponding inode and data block. Do not shift other inodes or file data blocks after deletion.

- `void fs_read(char name[5], int block_num)`
Opens the file with the given name and reads the block num-th block of the file into the buffer. 

- `void fs_write(char name[5], int block_num)`
Opens the file with the given name and writes the content of the buffer to the block num-th block of the file. 

- `void fs_buff(uint8_t buff[1024])`
Flushes the buffer by setting it to zero and writes the new bytes into the buffer.

- `void fs_ls(void)`
Lists all files and directories that exist in the current directory, including special directories . and .. which represent the current working directory and the parent directory of the current working directory, respectively. 

- `void fs_resize(char name[5], int new_size)`
Changes the size of the file with the given name to new size. If the new size is greater than the current size of the file, we allocate more blocks to this file. The start block fixed and new data blocks are added to the end such that the file’s data blocks are still contiguous. If there are enough free blocks after the last block of this file, the size is changed in the inode to new size. Otherwise, the file is moved so that you can increase the file size to the new size, and all data in the previous blocks are copied to the new blocks. The new starting block is the first available one that has enough space for the resized file.

- `void fs_defrag(void)`
Re-organizes the file blocks such that there is no free block between the used blocks, and between the superblock and the used blocks. To this end, starting with the file that has the smallest start block, the start block of every file must be moved over to the smallest numbered data block that is free.

- `void fs_cd(char name[5])`
Changes the current working directory to a directory with the specified name in the current working directory. This directory can be ., .., or any directory the user created on the disk.