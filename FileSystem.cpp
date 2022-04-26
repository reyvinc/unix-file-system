#include <string>
#include <fstream>
#include <sys/stat.h>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <tic.h>
#include <sstream>
#include "FileSystem.h"
#define ROOT 127
#define NUM_INODES 126
#define NUM_BLOCKS 16000
#define MOUNT_ERROR() std::cerr << "Error: No file system is mounted\n"
#define COMMAND_ERROR(file, line) std::cerr << "Command Error: " << file << ", " << line << std::endl
#define FILE_NOT_EXIST(file) std::cerr << "Error: File or directory " << file <<" does not exist\n"
#define FILE_EXIST(file) std::cerr << "Error: File or directory " << file <<" already exists\n"

bool mounted = false;
char buffer[1024];
Super_block *superblock;
uint8_t current_directory_int = ROOT;    // start as root
std::string current_disk;
char zeros[1024] = {0};

inline bool file_exists(char *name) {
    struct stat buffer;
    return (stat(name, &buffer) == 0);
}

inline bool block_marked_free(Super_block *sb, int index) {
    uint8_t val = index % 8;
    uint8_t mask = 1 << (7 - val);
    return !(sb->free_block_list[index / 8] & mask);
}

void set_block_free(int index) {
    uint8_t val = index % 8;
    uint8_t mask = 1 << (7 - val);
    mask = ~mask;
    superblock->free_block_list[index / 8] &= mask;
}

void set_block_used(int index) {
    uint8_t val = index % 8;
    uint8_t mask = 1 << (7 - val);
    superblock->free_block_list[index / 8] |= mask;
}

std::string &trim(std::string &str) {
    const std::string &chars = "\t\n\v\f\r ";
    str.erase(str.find_last_not_of(chars) + 1);
    str.erase(0, str.find_first_not_of(chars));
    return str;
}

void set_block_range_used(uint8_t start, uint8_t end) {
    // set new block as used
    for (unsigned int i = start; i < end; i++) {
        set_block_used(i);
    }
}

void set_block_range_free(uint8_t start, uint8_t end) {
    // set new block as used
    for (unsigned int i = start; i < end; i++) {
        set_block_free(i);
    }
}


uint8_t get_node_size(Inode &inode) {
    return inode.used_size & 0x7f;
}

inline bool node_in_use(Inode &inode) {
    return inode.used_size & 0x80;
}

inline bool is_directory(Inode &inode) {
    return inode.dir_parent & 0x80;
}

uint8_t get_parent_node_index(Inode inode) {
    return inode.dir_parent & 0x7f;
}

struct cmp_nodes {
    inline bool operator()(const int &first, const int &second) {
        return (superblock->inode[first].start_block < superblock->inode[second].start_block);
    }
};

Inode get_parent_node(Inode inode) {
    return superblock->inode[get_parent_node_index(inode)];
}

inline bool all_bytes_zero(Inode &inode) {
    return inode.dir_parent == 0 && inode.used_size == 0 && inode.start_block == 0
                && inode.name[0] == 0 && inode.name[1] == 0 && inode.name[2] == 0
                && inode.name[3] == 0 && inode.name[4] == 0;
}

bool operator==(Inode const & lhs, Inode const & rhs) {
    return get_parent_node_index(lhs) == get_parent_node_index(rhs) && strncmp(lhs.name, rhs.name, 5) == 0;
}

int consistency_check(Super_block *sb) {
    /*
     * Blocks that are marked free in the free-space list cannot be allocated to any file. Similarly, blocks
     * marked in use in the free-space list must be allocated to exactly one file.
     */
    std::unordered_set<uint8_t> allocated_blocks;
    for (unsigned int i = 0; i < NUM_INODES; i++) {
        Inode inode = sb->inode[i];
        if (!node_in_use(inode) || !(1 <= inode.start_block && inode.start_block < NUM_BLOCKS)) {
            continue;
        }

        for (uint8_t j = inode.start_block; j < inode.start_block + get_node_size(inode); j++) {
            if (block_marked_free(sb, j)) {
                // block marked as free, but is used by this file
                return 1;
            }
            if ((allocated_blocks.find(j) != allocated_blocks.end())) {
                // block already allocated to another file
                return 1;
            }
            allocated_blocks.insert(j); // check if a later block overlaps here
        }
    }

    // checks all blocks marked as used are actually used
    for (int i = 1; i < NUM_BLOCKS; i++){
        if (block_marked_free(sb, i) && allocated_blocks.find(i) != allocated_blocks.end()){
            return 1;
        }
        if (!block_marked_free(sb, i) && allocated_blocks.find(i) == allocated_blocks.end()){
            return 1;
        }
    }

    /*
     * The name of every file/directory must be unique in each directory.
     */
    std::unordered_set<std::string> file_names;
    for (unsigned int i = 0; i < NUM_INODES; i++) {
        Inode inode = sb->inode[i];

        if (!node_in_use(inode)) {
            continue;
        }

        int parent_idx = get_parent_node_index(inode);
        std::string path = std::to_string(parent_idx) + "/" + std::string(inode.name, 5);
        if (file_names.find(path) != file_names.end()) {
            return 2;
        }
        file_names.insert(path);
    }

    // if the inode is marked as free, all bits must be 0.
    // if marked as used, name must have one non-zero bit
    for (unsigned int i = 0; i < NUM_INODES; i++) {
        Inode inode = sb->inode[i];
        if (!node_in_use(inode) && !all_bytes_zero(inode)) {
            return 3;
        }
        if (node_in_use(inode) && !strncmp(inode.name, "", 5)) {
            return 3;
        }
    }

    //The start block of every inode that is marked as a file must have a value between 1 and 127
    //inclusive
    for (unsigned int i = 0; i < NUM_INODES; i++) {
        Inode inode = sb->inode[i];

        if (node_in_use(inode) && !is_directory(inode) && (inode.start_block < 1 || inode.start_block > 127)) {
            return 4;
        }
    }

    // The size and start block of an inode that is marked as a directory must be zero.
    for (unsigned int i = 0; i < NUM_INODES; i++) {
        Inode inode = sb->inode[i];
        if (node_in_use(inode) && is_directory(inode) && !(get_node_size(inode) == 0 && inode.start_block == 0)) {
            return 5;
        }
    }

    //For every inode, the index of its parent inode cannot be 126. Moreover, if the index of the parent inode
    //is between 0 and 125 inclusive, then the parent inode must be in use and marked as a directory.
    for (unsigned int i = 0; i < NUM_INODES; i++) {
        Inode inode = sb->inode[i];

        if (!node_in_use(inode)) {
            continue;
        }

        uint8_t parent_idx = get_parent_node_index(inode);

        if (parent_idx == ROOT) continue;

        Inode parent = get_parent_node(inode);
        if (parent_idx < 0 || parent_idx > 125 ||!node_in_use(parent) || !is_directory(parent)) {
            return 6;
        }
    }

    return 0;
}

int get_node_index(Inode inode) {
    for (unsigned int i = 0; i < NUM_INODES; i++) {
        if (superblock->inode[i] == inode) {
            return i;
        }
    }

    return -1;
}

int get_node_index(char name[5], int directory) {
    for (unsigned int i = 0; i < NUM_INODES; i++) {
        if (!strncmp(superblock->inode[i].name, name, 5) && get_parent_node_index(superblock->inode[i]) == directory) {
            return i;
        }
    }

    return -1;
}

inline bool file_in_directory(char name[5], int directory) {
    return get_node_index(name, directory) >= 0;
}

void write_superblock() {
    std::fstream file(current_disk, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(0, std::ios::beg);

    file.write(superblock->free_block_list, 16);
    file.write((char *) &superblock->inode, sizeof(superblock->inode));

    file.close();
}

void move_data(uint8_t old_start, uint8_t new_start, uint8_t size) {
    char to_copy[1024];
    std::fstream file(current_disk, std::ios::binary | std::ios::in | std::ios::out);

    for (uint8_t i = 0; i < size; i++) {
        file.seekg(1024 * (old_start + i));
        file.read(to_copy, 1024);

        file.seekp(1024 * (new_start + i));
        file.write(to_copy, 1024);

        memset(to_copy, 0, 1024);
        file.seekp(1024 * (old_start + i));
        file.write(zeros, 1024);
    }

    file.close();
}

void delete_file(Inode &inode) {
    int idx = get_node_index(inode);
    int size = get_node_size(inode);

    set_block_range_free(inode.start_block, inode.start_block + size);

    std::fstream file(current_disk, std::ios::binary | std::ios::in | std::ios::out);

    for (uint8_t i = inode.start_block; i < inode.start_block + size; i++){
        file.seekp(1024 * i);
        file.write(zeros, 1024);
    }

    file.close();

    memset(inode.name, 0, 5);
    inode.start_block = 0;
    inode.used_size = 0;
    inode.dir_parent = 0;

    superblock->inode[idx] = inode;
}

void delete_directory(Inode inode) {
    std::vector<Inode> nodes_to_delete;
    int idx = get_node_index(inode);

    for (unsigned int i = 0; i < NUM_INODES; i++) {
        if (get_parent_node_index(superblock->inode[i]) == idx) {
            nodes_to_delete.push_back(superblock->inode[i]);
        }
    }

    for (unsigned int i = 0; i < nodes_to_delete.size(); i++) {
        if (is_directory(nodes_to_delete[i])) {
            delete_directory(nodes_to_delete[i]);
        } else {
            delete_file(nodes_to_delete[i]);
        }
    }

    memset(inode.name, 0, 5);
    inode.start_block = 0;
    inode.used_size = 0;
    inode.dir_parent = 0;
    superblock->inode[idx] = inode;
}

int get_num_children(int index) {
    int cnt = 0;
    for (int i = 0; i < NUM_INODES; i++) {
        if (get_parent_node_index(superblock->inode[i]) == index) {
            cnt++;
        }
    }

    return cnt + 2;
}

int get_parent_num_children(int index) {
    if (index == ROOT) {
        return get_num_children(ROOT);
    }

    int idx = get_parent_node_index(superblock->inode[index]);
    return get_num_children(idx);
}

int find_contiguous_blocks(int size) {
    int found_so_far = 0;
    int start = -1;
    for (unsigned int i = 1; i <= NUM_BLOCKS; i++) {
        if (block_marked_free(superblock, i)) {
            found_so_far++;
            if (start == -1) {
                start = i;
            }
        } else {
            found_so_far = 0;
            start = -1;
        }
        if (found_so_far == size) {
            break;
        }
    }

    return start;
}

void fs_mount(char *new_disk_name) {
    // check if the virtual disk exists
    if (!file_exists(new_disk_name)) {
        std::cerr << "Error: Cannot find disk " << std::string(new_disk_name) << std::endl;
        return;
    }

    Super_block *sb = new Super_block;
    std::fstream file(new_disk_name, std::ios::in | std::ios::out | std::ios::binary);

    if (!file.good()) {
        std::cerr << "Couldn't open file" << std::endl;
        file.close();
        return;
    }

    file.read(sb->free_block_list, 16);

    for (unsigned int i = 0; i < NUM_INODES; i++) {
        file.read(sb->inode[i].name, 5);
        char rest_of_node[3];
        file.read(rest_of_node, 3);
        sb->inode[i].used_size = (uint8_t) rest_of_node[0];
        sb->inode[i].start_block = (uint8_t) rest_of_node[1];
        sb->inode[i].dir_parent = (uint8_t) rest_of_node[2];
    }

    file.close();

    int check = consistency_check(sb);

    if (check) {
        std::cerr << "Error: File system in " << new_disk_name << " is inconsistent (error code: " << check << ")\n";
        return;
    }

    mounted = true;
    superblock = sb;
    current_disk = std::string(new_disk_name);
}

void fs_create(char name[5], int size) {
    if (!mounted) {
        MOUNT_ERROR();
        return;
    }

    Inode inode;
    bool found = false;
    int idx = 0;
    for (unsigned int i = 0; i < NUM_INODES; i++) {
        if (!node_in_use(superblock->inode[i])) {
            found = true;
            inode = superblock->inode[i];
            idx = i;
            break;
        }
    }

    if (!found) {
        std::cerr << "Error: Superblock in disk " << current_disk << " is full, cannot create " << name << std::endl;
        return;
    }

    std::string str_name(name);
    trim(str_name);

    // name already exists or reserved name
    if (file_in_directory(name, current_directory_int) || !str_name.compare(".") || !str_name.compare("..")) {
        FILE_EXIST(str_name);
        return;
    }

    // check if directory
    if (size == 0)
    {
        strncpy(inode.name, str_name.c_str(), 5);
        inode.used_size =  0x80;
        inode.start_block = 0;
        inode.dir_parent = 0x80 | current_directory_int;

        superblock->inode[idx] = inode;
        return;
    }

    // find contiguous blocks for files
    found = false;
    int found_so_far = 0;
    int start = -1;
    for (unsigned int i = 1; i <= NUM_BLOCKS; i++) {
        if (block_marked_free(superblock, i)) {
            found_so_far++;
            if (start == -1) {
                start = i;
            }
        } else {
            found_so_far = 0;
            start = -1;
        }
        if (found_so_far == size) {
            found = true;
            break;
        }
    }

    if (!found) {
        std::cerr << "Error: Cannot allocate " << size << " on " << current_disk << std::endl;
        return;
    }

    set_block_range_used(start, start + size);

    strncpy(inode.name, str_name.c_str(), 5);
    inode.used_size = 0x80 | size;
    inode.start_block = (uint8_t) start;
    inode.dir_parent = current_directory_int;

    superblock->inode[idx] = inode;
}

void fs_delete(char name[5]) {
    if (!mounted) {
        MOUNT_ERROR();
        return;
    }

    std::string s(name);
    trim(s);

    if (!file_in_directory(name, current_directory_int)) {
        FILE_NOT_EXIST(s);
        return;
    }

    int idx = get_node_index(name, current_directory_int);
    Inode inode = superblock->inode[idx];

    if (is_directory(inode)) {
        delete_directory(inode);
    } else {
        delete_file(inode);
    }

}

void fs_read(char name[5], int block_num) {
    if (!mounted) {
        MOUNT_ERROR();
        return;
    }

    std::string s(name);
    trim(s);

    if (!file_in_directory(name, current_directory_int)) {
        FILE_NOT_EXIST(s);
        return;
    }

    int idx = get_node_index(name, current_directory_int);
    Inode inode = superblock->inode[idx];

    if (block_num < 0 || block_num >= get_node_size(inode)) {
        std::cerr << "Error: " << s << " does not have block " << block_num << std::endl;
        return;
    }

    std::fstream file(current_disk, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(1024 * (inode.start_block + block_num));
    file.read(buffer, 1024);
    file.close();
}

void fs_write(char name[5], int block_num) {
    if (!mounted) {
        MOUNT_ERROR();
        return;
    }

    std::string s(name);
    trim(s);

    int idx = get_node_index(name, current_directory_int);
    if (idx == -1) {
        FILE_NOT_EXIST(s);
        return;
    }

    Inode inode = superblock->inode[idx];

    if (block_num < 0 || block_num >= get_node_size(inode)) {
        std::cerr << "Error: " << s << " does not have block " << block_num << std::endl;
        return;
    }

    std::fstream file(current_disk, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(1024 * (inode.start_block + block_num));
    file.write(buffer, 1024);
    file.close();
}

void fs_buff(char buff[1024]) {
    if (!mounted) {
        MOUNT_ERROR();
        return;
    }

    memcpy(buffer, buff, 1024);
}

void fs_ls(void) {
    if (!mounted) {
        MOUNT_ERROR();
        return;
    }

    Inode inode;
    if (current_directory_int != ROOT) inode = superblock->inode[current_directory_int];

    printf("%-5.5s %3d\n", ".", (int) get_num_children(current_directory_int));
    printf("%-5.5s %3d\n", "..", (int) get_parent_num_children(current_directory_int));

    for (int i = 0; i < NUM_INODES; i++) {
        if (get_parent_node_index(superblock->inode[i]) == current_directory_int) {
            if (is_directory(superblock->inode[i])) {
                printf("%-5.5s %3d\n", superblock->inode[i].name, get_num_children(i));
            } else {
                printf("%-5.5s %3d KB\n", superblock->inode[i].name, get_node_size(superblock->inode[i]));
            }
        }
    }
}

void fs_resize(char name[5], int new_size) {
    if (!mounted) {
        MOUNT_ERROR();
        return;
    }

    std::string str_name(name);
    trim(str_name);

    int idx = get_node_index(name, current_directory_int);
    if (idx == -1) {
        FILE_NOT_EXIST(str_name);
        return;
    }

    Inode inode = superblock->inode[idx];
    int size =  get_node_size(inode);
    std::fstream file(current_disk, std::ios::binary | std::ios::in | std::ios::out);

    if (new_size < size) {
        set_block_range_free(inode.start_block + new_size, inode.start_block + size);

        for (uint8_t i = inode.start_block + new_size; i < inode.start_block + size; i++){
            file.seekp(1024 * i);
            file.write(zeros, 1024);
        }

        file.close();
        inode.used_size = 0x80 | new_size;

        superblock->inode[idx] = inode;
        return;
        //TODO
    }

    bool fits_original_position = true;
    for (uint8_t j = inode.start_block + size; j < inode.start_block + new_size; j++) {
        if (!block_marked_free(superblock, j)) {
            fits_original_position = false;
            break;
        }
    }

    if (fits_original_position) {
        inode.used_size = 0x80 | new_size;
        superblock->inode[idx] = inode;

        set_block_range_used(inode.start_block, inode.start_block + new_size);
    } else {
        bool found = false;
        int found_so_far = 0;
        int start = -1;
        for (unsigned int i = 1; i <= 128; i++) {
            if (block_marked_free(superblock, i) || (i >= inode.start_block && i < inode.start_block + size)) {
                found_so_far++;
                if (start == -1) {
                    start = i;
                }
            } else {
                found_so_far = 0;
                start = -1;
            }
            if (found_so_far == new_size) {
                found = true;
                break;
            }
        }

        if (!found) {
            std::cerr << "Error: File " << str_name << " cannot expand to size " << new_size;
            return;
        }

        // set old block as free
        set_block_range_free(inode.start_block, inode.start_block + size);

        // set new block as used
        set_block_range_used(start, start + new_size);

        move_data(inode.start_block, start, size);

        inode.start_block = start;
        inode.used_size = 0x80 | new_size;
        superblock->inode[idx] = inode;
    }
}

void fs_defrag(void) {
    if (!mounted) {
        MOUNT_ERROR();
        return;
    }

    std::vector<int> nodes;
    for (int i = 0; i < NUM_INODES; i++) {
        if (node_in_use(superblock->inode[i]) && !is_directory(superblock->inode[i])) {
            nodes.push_back(i);
        }
    }

    if (nodes.empty()) return;
    std::sort(nodes.begin(), nodes.end(), cmp_nodes());

    int i = 1;
    int j = 0;

    std::fstream file(current_disk, std::ios::binary | std::ios::in | std::ios::out);
    while (i <= NUM_BLOCKS && j < nodes.size()) {
        if (block_marked_free(superblock, i)) {
            Inode inode = superblock->inode[nodes[j]];
            uint8_t start = inode.start_block;
            uint8_t size = get_node_size(inode);

            move_data(start, i, size);

            set_block_range_free(start, start + size);
            set_block_range_used(i, i + size);

            inode.start_block = i;
            superblock->inode[nodes[j]] = inode;

            j += 1;
            i += size - 1;
        }
        i++;
    }
    file.close();
}

void fs_cd(char name[5]) {
    if (!mounted) {
        MOUNT_ERROR();
        return;
    }

    std::string dir(name);
    if (!dir.compare(".")) {
        return;
    }

    if (!dir.compare("..")) {
        if (current_directory_int == ROOT) {
            // don't do anything
            return;
        }

        current_directory_int = get_parent_node_index(superblock->inode[current_directory_int]);
        return;
    }

    int idx = get_node_index(name, current_directory_int);
    if (idx == -1) {
        std::cerr << "Error: Directory "<< dir << " does not exist\n";
        return;
    }

    current_directory_int = idx;
}

void run_commands(std::string input_file) {
    std::ifstream infile(input_file);
    std::string line;
    long line_number = 0;

    while (std::getline(infile, line)) {
        line_number++;
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (!cmd.compare("M")) {
            std::string disk_name;
            iss >> disk_name;

            if (iss.fail() || !iss.eof()) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            fs_mount((char * ) disk_name.c_str());
        } else if (!cmd.compare("C")) {
            std::string file_name;
            int size;
            iss >> file_name >> size;

            if (iss.fail() || !iss.eof()) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            trim(file_name);
            if (file_name.length() > 5 || !(0 <= size && size <= 127)) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            fs_create((char * ) file_name.c_str(), size);
        } else if (!cmd.compare("D")) {
            std::string file_name;
            iss >> file_name;

            if (iss.fail() || !iss.eof()) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            trim(file_name);
            if (file_name.length() > 5) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            fs_delete((char * ) file_name.c_str());
        } else if (!cmd.compare("R")) {
            std::string file_name;
            int block_num;
            iss >> file_name >> block_num;

            if (iss.fail() || !iss.eof() || !(1 <= block_num && block_num <= 127)) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            trim(file_name);
            if (file_name.length() > 5) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            fs_read((char * ) file_name.c_str(), block_num);
        } else if (!cmd.compare("W")) {
            std::string file_name;
            int block_num;
            iss >> file_name >> block_num;

            if (iss.fail() || !iss.eof() || !(0 <= block_num && block_num <= 127)) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            trim(file_name);
            if (file_name.length() > 5) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            fs_write((char * ) file_name.c_str(), block_num);
        } else if (!cmd.compare("B")) {
            std::string word;
            iss >> word;
            if (line.length() < 3 || !word.length()) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            std::string in = line.substr(2);
            if ( in.length() > 1000) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            char buff[1024] = {0};
            std::strcpy(buff, in .c_str());
            fs_buff(buff);
        } else if (!cmd.compare("L")) {
            if (!iss.eof()) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            fs_ls();
        } else if (!cmd.compare("E")) {
            std::string file_name;
            int new_size;
            iss >> file_name >> new_size;

            if (iss.fail() || !iss.eof() || new_size < 1) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            trim(file_name);
            if (file_name.length() > 5) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            fs_resize((char * ) file_name.c_str(), new_size);
        } else if (!cmd.compare("O")) {
            if (!iss.eof()) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            fs_defrag();
        } else if (!cmd.compare("Y")) {
            std::string file_name;
            iss >> file_name;

            if (iss.fail() || !iss.eof()) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            trim(file_name);
            if (file_name.length() > 5) {
                COMMAND_ERROR(input_file, line_number);
                continue;
            }

            fs_cd((char * ) file_name.c_str());
        }
        write_superblock();
    }
}

int main (int argc, char *argv[]) {
    run_commands(std::string("/Users/ahmed/CLionProjects/untitled/consistency-input"));
    return 0;
}
