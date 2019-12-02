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

char buffer[1024];
Super_block* superblock;
std::unordered_map<std::string, Inode> current_directory;
uint8_t current_directory_int = ROOT;    // start as root
std::string current_disk;

std::ofstream &operator << (std::ofstream &s, Inode const& node)
{
    s << node.name << node.used_size << node.start_block << node.dir_parent;
    return s;
}

inline bool file_exists (char* name) {
    struct stat buffer;
    return (stat (name, &buffer) == 0);
}

// 0 if free
inline bool block_marked_free(Super_block* sb, int index) {
    uint8_t mask = 1 << (index % 8);
    return !(sb->free_block_list[index / 16] & mask);
}

void set_block_free(Super_block* sb, int index) {
    uint8_t mask = 1 << index;
    mask = ~mask;
    sb->free_block_list[index / 16] &= mask;
}

void set_block_used(Super_block* sb, int index) {
    uint8_t mask = 1 << index;
    sb->free_block_list[index / 16] |= mask;
}

// 1 = in use
inline bool node_in_use(Inode inode) {
    return inode.used_size & 0x80;
}

uint8_t get_inode_size(Inode inode) {
    // 0 if directory
    return inode.used_size & 0x7f;
}

inline bool is_directory(Inode inode) {
    // 1: directory
    // 0: file
    return inode.dir_parent & 0x80;
}

uint8_t get_parent_node_index(Inode inode) {
    // 0 to 125 inclusive
    // 126: error
    // 127: root directory
    return inode.dir_parent & 0x7f;
}

Inode get_parent_node(Inode node) {
    return superblock->inode[get_parent_node_index(node)];
}

inline bool all_bytes_zero(Inode inode) {
    uint8_t val = inode.dir_parent || inode.used_size || inode.name[0] || inode.start_block;
    return val == 0;
}


std::unordered_set<int> free_blocks;
std::unordered_set<int> used_blocks;

void check_allocation(Super_block* sb) {
    for (uint8_t i = 0; i < 128; i++)
    {
        if (block_marked_free(sb, i))
        {
            free_blocks.insert(i);
        }
        else
        {
            used_blocks.insert(i);
        }
    }
}

int consistency_check(Super_block* sb) {
    /*
     * Blocks that are marked free in the free-space list cannot be allocated to any file. Similarly, blocks
     * marked in use in the free-space list must be allocated to exactly one file.
     */

    std::unordered_set<uint8_t> allocated_blocks;
    for (unsigned int i = 0; i < 126; i++)
    {
        Inode node = sb->inode[i];
        if (!node_in_use(node))
        {
            continue;
        }

        for (uint8_t j = node.start_block; j < node.start_block + get_inode_size(node); j++)
        {
            if (block_marked_free(sb, j))
            {
                // block marked as free, but is used by this file
                return 1;
            }
            if ((allocated_blocks.find(j) != allocated_blocks.end()))
            {
                // block already allocated to another file
                return 1;
            }
            allocated_blocks.insert(j); // check if a later block overlaps here
//            used_blocks.erase(j);
        }
    }
    //TODO: Check for files that are marked as used but arent used
//    if (used_blocks.size())
//    {
//        // blocks marked as used but not allocated
//        return 1;
//    }

    /*
     * The name of every file/directory must be unique in each directory.
     */
    std::unordered_set<std::string> file_names;
    for (unsigned int i = 0; i < 126; i++) {
        Inode node = sb->inode[i];

        if (!node_in_use(node))
        {
            continue;
        }


        uint8_t parent_index = get_parent_node_index(node);
        Inode parent_node = sb->inode[parent_index];
        std::string path = std::string(parent_node.name) + "/" + std::string(node.name);
        if (file_names.find(path) != file_names.end())
        {
            return 2;
        }
        file_names.insert(path);
    }

    for (unsigned int i = 0; i < 126; i++) {
        Inode node = sb->inode[i];
        if (!node_in_use(node) && !all_bytes_zero(node))
        {
            return 3;
        }
        if (node_in_use(node) && !node.name[0])
        {
            return 3;
        }
    }

    for (unsigned int i = 0; i < 126; i++) {
        Inode node = sb->inode[i];

        if (node_in_use(node) && !is_directory(node) && (node.start_block < 1 || node.start_block > 127))
        {
            return 4;
        }
    }

    for (unsigned int i = 0; i < 126; i++) {
        Inode node = sb->inode[i];
        if (node_in_use(node) && is_directory(node) && !(get_inode_size(node) == 0 && node.start_block == 0))
        {
            return 5;
        }
    }

    for (unsigned int i = 0; i < 126; i++) {
        Inode node = sb->inode[i];

        if (!node_in_use(node))
        {
            continue;
        }

        uint8_t parent_idx = get_parent_node_index(node);
        Inode parent = sb->inode[parent_idx];
        if (parent_idx == 126 || !node_in_use(parent) || !is_directory(parent))
        {
            return 6;
        }
    }

    return 0;
}

void fs_mount(char *new_disk_name) {
    if (!file_exists(new_disk_name))
    {
        std::cerr << "Error: Cannot find disk " << *new_disk_name << std::endl;
    }

    Super_block* sb = new Super_block;
    std::fstream file (new_disk_name, std::ios::in | std::ios::out | std::ios::binary);
    char a[16];
    file.read (sb->free_block_list, 16);

    for (unsigned int i = 0; i < 126; i++)
    {
        file >> sb->inode[i].name >> sb->inode[i].used_size >> sb->inode[i].start_block >>
            sb->inode[i].dir_parent;
    }

    int check = consistency_check(sb);

    if (check)
    {
        std::cerr << "Error: File system in " << new_disk_name << " is inconsistent (error code: " << check << ")";
    }

    superblock = sb;
    file.close();

    current_disk = std::string(new_disk_name);
}

std::string& trim(std::string& str)
{
    const std::string& chars = "\t\n\v\f\r ";
    str.erase(str.find_last_not_of(chars) + 1);
    str.erase(0, str.find_first_not_of(chars));
    return str;
}

void fs_create(char name[5], int size) {
    Inode node;
    bool found = false;
    int idx = 0;
    for (unsigned int i = 0; i < 126; i++)
    {
        if (!node_in_use(superblock->inode[i]))
        {
            found = true;
            node = superblock->inode[i];
            idx = i;
            break;
        }
    }
    if (!found)
    {
        std::cerr << "Error: Superblock in disk " << current_disk << " is full, cannot create " << name << std::endl;
    }

    std::string str_name(name);
    trim(str_name);

    if (current_directory.find(str_name) != current_directory.end())
    {
        std::cerr << "Error: File or directory <file name> already exists";
    }

    found = false;
    int found_so_far = 0;
    int start = -1;
    for (unsigned int i = 1; i <= 128; i++)
    {
        if (block_marked_free(superblock, i))
        {
            found_so_far++;
            if (start == -1)
            {
                start = i;
            }
        }
        else
        {
            found_so_far = 0;
            start = -1;
        }
        if (found_so_far == size)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        std::cerr << "Cannot allocate file size";
    }
    strcpy(node.name, str_name.c_str());
    node.used_size = size;
    node.used_size |= 0x80;

    node.start_block = (uint8_t) start;
    node.dir_parent = current_directory_int;
    if (size == 0)
    {
        // if directory-> MSB is 1
        node.dir_parent |= 0x80;
    }
    current_directory[str_name] = node;
    superblock->inode[idx] = node;
}

void delete_file(Inode node){
    for (uint8_t j = node.start_block; j < node.start_block + get_inode_size(node); j++)
    {
        set_block_free(superblock, j);
    }

    memset(node.name, 0, 5);
    node.start_block = 0;
    node.used_size = 0;
    node.dir_parent = 0;
}

void delete_directory(Inode node){
    std::vector<Inode> nodes_to_delete;
    int idx;
    // get index of current node
    for (unsigned int  i = 0; i < 126; i++){
        if (superblock->inode[i].start_block == node.start_block)
        {
            idx = i;
        }
    }

    for (unsigned int  i = 0; i < 126; i++){
        if (get_parent_node_index(superblock->inode[i]) == idx)
        {
            nodes_to_delete.push_back(superblock->inode[i]);
        }
    }

    for (unsigned int i = 0; i < nodes_to_delete.size(); i++) {
        if (is_directory(node))
        {
            delete_directory(node);
        }
        else
        {
            delete_file(node);
        }
    }
}

void fs_delete(char name[5]) {
    std::string s(name);
    trim(s);

    if (current_directory.find(s) == current_directory.end()) {
        std::cerr << "Error: File or directory " << s << " does not exist\n";
    }

    Inode node = current_directory[s];
    if (is_directory(node))
    {
        delete_directory(node);
    }
    else
    {
        delete_file(node);
    }
}

void fs_read(char name[5], int block_num) {
    std::string s(name);

    if (current_directory.find(s) == current_directory.end()) {
        std::cerr << "Error: File or directory " << s << " does not exist\n";
    }

    Inode node = current_directory[s];

    std::fstream file (current_disk, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(1024 * (node.start_block+block_num));
    file.read(buffer, 1024);
    file.close();
}

void fs_write(char name[5], int block_num) {
    std::string s(name);

    if (current_directory.find(s) == current_directory.end()) {
        std::cerr << "Error: File or directory " << s << " does not exist\n";
    }

    Inode node = current_directory[s];

    std::fstream file (current_disk, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(1024 * (node.start_block+block_num));
    file.write(buffer, 1024);
    file.close();
}

void fs_buff(char buff[1024]) {
    memcpy(buffer, buff, 1024);
//    for (int i = 0; i < 1024; i++)
//    {
//        buffer[i] = buff[i];
//    }
}

void fs_ls(void);
void fs_resize(char name[5], int new_size);
void fs_defrag(void);
void fs_cd(char name[5]) {
    std::string dir(name);
    if (!dir.compare("."))
    {
        return;
    }
    if (!dir.compare(".."))
    {
        if (current_directory_int == ROOT)
        {
            return;
        }

        int parent_idx = get_parent_node_index(superblock->inode[current_directory_int]);
        Inode parent = superblock->inode[parent_idx];
        current_directory.clear();

        for (int i = 0; i < 126; i++)
        {
            if (get_parent_node_index(superblock->inode[i]) == parent_idx)
            {
                current_directory[std::string(superblock->inode[i].name)] = superblock->inode[i];
            }
        }

        current_directory_int = parent_idx;
        return;
    }
    if (current_directory.find(dir) == current_directory.end())
    {
        std::cerr << "Error: Directory <directory name> does not exist";
    }

    Inode node = current_directory[dir];
    current_directory.clear();
    int idx;
    // get index of current node
    for (unsigned int  i = 0; i < 126; i++){
        if (superblock->inode[i].start_block == node.start_block)
        {
            idx = i;
        }
    }

    for (unsigned int  i = 0; i < 126; i++) {
        if (get_parent_node_index(superblock->inode[i]) == idx) {
            current_directory[std::string(superblock->inode[i].name)] = superblock->inode[i];
        }
    }

    current_directory_int = idx;
}

void write_superblock() {
    std::ofstream file (current_disk, std::ios::binary);
    file.seekp(0, std::ios::beg);
    file.write(superblock->free_block_list, 16);
    file.write((char*)superblock->inode, 1008);

//    for (unsigned int i = 0; i < 126; i++)
//    {
//        file.write(superblock->inode[i].name, 5);
//        file.write(superblock->inode[i].used_size, 1);
//        file.write(superblock->inode[i].start_block, 1);
//        file.write(superblock->inode[i].dir_parent, 1);
//    }

    file.close();
}

int main() {
    std::ifstream infile("/Users/ahmed/CLionProjects/untitled/input1");
    std::string line;
    while (std::getline(infile, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        std::cout << cmd;
        if (!cmd.compare("M"))
        {
            std::string disk_name;
            iss >> disk_name;
            fs_mount((char*) disk_name.c_str());
        }
        else if (!cmd.compare("C"))
        {
            std::string file_name;
            int size;
            iss >> file_name >> size;
            fs_create((char*) file_name.c_str(), size);
        }
        else if (!cmd.compare("D"))
        {
            std::string file_name;
            iss >> file_name;
            fs_delete((char*) file_name.c_str());
        }
        else if (!cmd.compare("R"))
        {
            std::string file_name;
            int size;
            iss >> file_name >> size;
            fs_read((char*) file_name.c_str(), size);
        }
        else if (!cmd.compare("W"))
        {
            std::string file_name;
            int size;
            iss >> file_name >> size;
            fs_write((char*) file_name.c_str(), size);
        }
        else if (!cmd.compare("B"))
        {
            std::string in;
            iss >> in;
            char buff[1024] = {0};
            std::strcpy (buff, in.c_str());
            fs_buff(buff);
        }
        else if (!cmd.compare("L"))
        {

        }
        else if (!cmd.compare("E"))
        {

        }
        else if (!cmd.compare("O"))
        {

        }
        else if (!cmd.compare("Y"))
        {
            std::string file_name;
            iss >> file_name;
            fs_cd((char*) file_name.c_str());
        }

        write_superblock();
        std::cout << std::endl;
    }
    return 0;
}