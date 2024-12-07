# define FUSE_USE_VERSION 29


# include <fuse.h>
# include <iostream>
# include <fstream>
# include <sstream>
# include <vector>
# include <string>
# include <unistd.h>
# include <sys/types.h>
# include <set>

#define NDEBUG
# include <assert.h>

# include <map>
# include <algorithm>


#ifndef BLOCK_SIZE
    #define BLOCK_SIZE 16384
#endif

#define BLOCK_NUMBER_SIZE 4
#define BLOCK_NUMBERS_PER_BLOCK (BLOCK_SIZE / BLOCK_NUMBER_SIZE)
#define MAX_BLOCKS (1UL << (BLOCK_NUMBER_SIZE*8))-1

/*
 * Pointer block simply points to a list of blocks, and to the next pointer block.
*/
struct pointer_block {
	uint32_t next_pointer_block; // Valid if > 0
	uint32_t num_pointers; // In this block
	uint32_t blocks[BLOCK_NUMBERS_PER_BLOCK-2];
};

/* File contents */
struct data_block {
	char data[BLOCK_SIZE];
};


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



struct b_file {
	std::string name;
	long size;
	uint32_t parent_block;

	std::vector<uint32_t> data_blocks;

	bool dirty_metadata;
	std::set<uint32_t> deleted_blocks;
};

static void describe(b_file* f) {
	return;
	std::cout << "File " << f->name << " has " << f->data_blocks.size() << " blocks." << std::endl;

	for (int i = 0; i < std::min(static_cast<int>( f->data_blocks.size()), 3); i++) {
		std::cout << "Block " << i << ": " << f->data_blocks[i] << std::endl;
	}
	/* std::cout << "First block: " << f->data_blocks[0] << std::endl; */
	std::cout << "Size: " << f->size << std::endl;
	std::cout << "Expected blocks: " << (f->size + BLOCK_SIZE - 1) / BLOCK_SIZE << std::endl;
}

struct b_directory {
	std::string name;
	long size;
	uint32_t parent_block; // Is 0 for root

	std::vector<uint32_t> directory_blocks;

	std::map<std::string, b_file*> files;
	std::map<std::string, b_directory*> directories;
};

static void describe(b_directory* root) {
	return;
	/* std::cout <<std::endl << std::endl; */
	std::cout << "Directory " << root->name << " has " << root->directories.size() << " directories and " << root->files.size() << " files." << std::endl;
	for (auto it = root->directories.begin(); it != root->directories.end(); it++) {
		std::cout << "Directory " << it->first << " has " << it->second->directories.size() << " directories and " << it->second->files.size() << " files." << std::endl;
	}
	for (auto it = root->files.begin(); it != root->files.end(); it++) {
		std::cout << "File " << it->first << " has " << it->second->data_blocks.size() << " blocks." << std::endl;
	}

	assert(root->directory_blocks.size() > 0);
	assert(root->directory_blocks[0] < 10);

	/* std::cout <<std::endl << std::endl; */
}

struct sffs_state {
	std::string file_name;
	std::fstream file;
	b_directory* root;
	std::set<std::pair<uint32_t, uint32_t> > free_block_ranges; 
	int available_file_handle;
	std::map<int, b_file*> open_files;
	std::map<uint32_t, data_block> block_cache;
};
#define STATE ((struct sffs_state *) fuse_get_context()->private_data)

static struct options {
	const char *file_name;
} options;
#define OPTION(t,p) {t, offsetof(struct options, p), 1}

static const struct fuse_opt option_spec[] = {
	OPTION("--file_name=%s", file_name),
	FUSE_OPT_END
};






// (inclusive, inclusive)
// Once a block is allocated, it is removed from the set
inline uint32_t get_next_block() {
    assert(!STATE->free_block_ranges.empty()); // Ensure we have blocks to allocate

    auto range = STATE->free_block_ranges.begin();
    uint32_t start = range->first;
    uint32_t end = range->second;

    STATE->free_block_ranges.erase(range);
    if (start < end) {
        STATE->free_block_ranges.insert(std::make_pair(start + 1, end));
    }
	/* std::cout << "Allocating block " << start << std::endl; */
    return start;
}


inline void read_block(std::fstream& file, uint32_t block_number, char* block, size_t size) {
	auto offset = block_number * BLOCK_SIZE;
	/* std::cout << "Reading " << size << " bytes from block " << block_number << " at offset " << offset << "\n"; */
	assert(file.good());

	file.seekg(offset);
	file.read(block, size);
	assert(file.good());
	assert(file.read(block, size));
	assert(file.good());
}


inline void write_block(std::fstream& file, uint32_t block_number, void* block) {
    auto offset = block_number * BLOCK_SIZE;

    /* std::cout << "Writing block " << block_number << " at offset " << offset << std::endl; */

    // Ensure the stream is in a valid state
	if (!file.good()) {
		std::cout << "Stream is not good before operation!" << std::endl;
		exit(1);
	}

    // Check the current file size
    file.seekp(0, std::ios::end);
    auto current_size = file.tellp();
	if (current_size == -1) {
		std::cout << "Failed to determine file size!" << std::endl;
		exit(1);
	}

    // Seek to the target offset
    file.seekp(offset);
	if (!file.good()) {
		std::cout << "Stream is not good after seek!" << std::endl;
		exit(1);
	}

    // Verify the position before writing
    /* std::cout << "Start: " << file.tellp() << std::endl; */

    // Write the block
	file.write(reinterpret_cast<const char*>(block), BLOCK_SIZE);
	if (!file.good()) {
		std::cout << "Failed to write block!" << std::endl;
		exit(1);
	}

    // Verify the position after writing
    auto end = file.tellp();
	if (end == -1) {
		std::cout << "Failed to determine position after writing!" << std::endl;
		exit(1);
	}

	/* std::cout << "Block written successfully at offset " << offset << std::endl; */
}



inline void add_empty_entry(std::fstream& file, b_directory& root, std::string name, bool is_dir) {
	if (root.directories.find(name) != root.directories.end()) {
		std::cout << "Directory " << name << " already exists." << std::endl;
		return;
	}
	if (root.files.find(name) != root.files.end()) {
		std::cout << "File " << name << " already exists." << std::endl;
		return;
	}

	std::cout << "Adding empty entry " << name << std::endl;
	describe(&root);

	descriptor desc = {};
	desc.type = is_dir ? 1 : 0;
	strcpy(desc.name, name.c_str());
	desc.size = 0;

	uint32_t block_number = root.directory_blocks.at(0); // Root's block
	uint32_t new_block_number;
	if (is_dir) {
		new_block_number = get_next_block();
		desc.blocks[0] = new_block_number;
		std::cout << "Creating new directory" << std::endl;
		directory_block new_block = {};
		write_block(file, new_block_number, &new_block);

		b_directory* new_dir = new b_directory();
		new_dir->name = name;
		new_dir->parent_block = block_number;
		new_dir->size = 0;
		new_dir->directory_blocks.push_back(new_block_number);
		new_dir->files = {};
		new_dir->directories = {};
		describe(new_dir);
		root.directories[name] = new_dir;
	} else {
		std::cout << "Creating new file" << std::endl;
		/* data_block new_data_block = {}; */
		/* write_block(file, new_block_number, &new_data_block); */

		b_file* new_file = new b_file();
		new_file->name = name;
		new_file->parent_block = block_number;
		new_file->size = 0;
		/* new_file->data_blocks.push_back(new_block_number); */

		root.files[name] = new_file;
	}

	describe(&root);


	std::cout << "Updating directory block " << block_number << std::endl;

	while(true){
		directory_block db;
		read_block(file, block_number, (char*)&db, sizeof(directory_block));
	
		// 3.1. Find a free slot
		std::cout << "Looking for free slot" << std::endl;
		for (int i = 0; i < db.num_entries; i++) {
			if (db.descriptors[i].blocks[0] == 0) {
				db.descriptors[i] = desc;
				write_block(file, block_number, &db);
				return;
			}
		}

		// 3.2. If no free slot, try to insert at the end
		std::cout << "No free slot, trying to insert at the end" << std::endl;
		if (db.num_entries < sizeof(db.descriptors)/sizeof(descriptor)) {
			db.descriptors[db.num_entries] = desc;
			db.num_entries = db.num_entries + 1;
			write_block(file, block_number, &db);
			return;
		}

		// 3.3. If no next block, create a new one and insert there
		std::cout << "Checking if there is a next block" << std::endl;
		if (db.next_block == 0) {
			uint32_t new_block_number = get_next_block();
			directory_block new_db = {};
			new_db.num_entries = 1;
			new_db.next_block = 0;
			new_db.descriptors[0] = desc;

			db.next_block = new_block_number;
			root.directory_blocks.push_back(new_block_number);

			write_block(file, new_block_number, &new_db);
			write_block(file, block_number, &db);
			return;
		}

		/* 3.4. If no free slot and no space, go to next block */
		std::cout << "Going to next block" << std::endl;
		block_number = db.next_block;
	}
}


inline std::vector<uint32_t> decode_pointer_block(std::fstream& file, uint32_t block_number, std::set<uint32_t>& visited) {
	std::cout << "Decoding pointer block " << block_number << std::endl;
	pointer_block pb;
	read_block(file, block_number, (char*)&pb, sizeof(pointer_block));
	visited.insert(block_number);

	std::vector<uint32_t> blocks;
	for (int i = 0; i < pb.num_pointers; i++) {
		assert(pb.blocks[i] > 0);
		visited.insert(pb.blocks[i]);
		blocks.push_back(pb.blocks[i]);
	}

	if (pb.next_pointer_block > 0) {
		auto next = decode_pointer_block(file, pb.next_pointer_block, visited);
		blocks.insert(blocks.end(), next.begin(), next.end());
	}

	return blocks;
}


// A directory is defined entirely by a block number (outside of it's name)
// This function builds the entire directory structure from a block number
inline b_directory* decode_directory(std::fstream& stream, uint32_t block_number, std::set<uint32_t>& visited) {
	std::cout << "Decoding directory " << block_number << std::endl;

	visited.insert(block_number);
	/* std::cout << "Visited " << visited.size() << " blocks." << std::endl; */
	b_directory* res = new b_directory();
	res->files = {};
	res->directories = {};
	res->directory_blocks.push_back(block_number);

	directory_block directory = {};
	read_block(stream, block_number, (char*) &directory, sizeof(directory_block));

	for (int i = 0; i < directory.num_entries; i++) {
		descriptor desc = directory.descriptors[i];
		if (desc.blocks[0] == 0) {
			// Invalid entry, probably deleted
			continue;
		}

		if (desc.type == 0) {
			b_file* file = new b_file();
			file->name = desc.name;
			file->size = desc.size;
			file->parent_block = block_number;

			int expected_blocks = (desc.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
			int blocks_in_descriptor = sizeof(desc.blocks)/sizeof(uint32_t);
			for (int j = 0; j < std::min(blocks_in_descriptor, expected_blocks); j++) {
				if (desc.blocks[j] > 0) {
					visited.insert(desc.blocks[j]);
					file->data_blocks.push_back(desc.blocks[j]);
				}
			}
			if (desc.pointer_block > 0) {
				auto blocks = decode_pointer_block(stream, desc.pointer_block, visited);
				file->data_blocks.insert(file->data_blocks.end(), blocks.begin(), blocks.end());
			}
			res->files[file->name] = file;
			describe(file);
		} else {
			b_directory* next = decode_directory(stream, desc.blocks[0], visited);
			next->name = desc.name;
			next->size = desc.size;
			next->parent_block = block_number;
			res->directories[next->name] = next;
		}
	}


	if (directory.next_block > 0) {
		b_directory* next = decode_directory(stream, directory.next_block, visited);
		assert(res->name == next->name);
		assert(res->size == next->size);
		assert(res->parent_block == next->parent_block);

		res->files.insert(next->files.begin(), next->files.end());
		res->directories.insert(next->directories.begin(), next->directories.end());
		res->directory_blocks.insert(res->directory_blocks.end(), next->directory_blocks.begin(), next->directory_blocks.end());
	}

	/* describe(res); */
	return res;
};


// There are num_pointers that are valid in the chain
// Take care of the rest
void free_pointer_blocks(int pointer_block_number, int num_pointers) {
	pointer_block pb;
	read_block(STATE->file, pointer_block_number, (char*)&pb, sizeof(pointer_block));

	if(num_pointers == 0) {
		STATE->free_block_ranges.insert(std::make_pair(pointer_block_number, pointer_block_number));
		return free_pointer_blocks(pb.next_pointer_block, 0);
	}

	// Are there too many pointers in this block?
	if (pb.num_pointers > num_pointers) {
		pb.num_pointers = num_pointers;
		write_block(STATE->file, pointer_block_number, &pb);

		return free_pointer_blocks(pb.next_pointer_block, 0);
	}

	assert(false);
	// This should never happen
}


void write_file_metadata(b_file* f) {
	std::cout << "Writing metadata for file " << f->name << std::endl;

	// 1. Write the file metadata
	descriptor desc = {};
	
	auto parent_block = f->parent_block;
	directory_block db = {};
	int index = 0;
	bool found = false;

 	auto iteration_count = 0;
	while(!found) {
		iteration_count++;
		if (iteration_count > 30) {
			std::cout << "Infinite loop detected" << std::endl;
			exit(1);
		}
		std::cout << "Looking for descriptor in block " << parent_block << std::endl;
		read_block(STATE->file, parent_block, (char*)&db, sizeof(directory_block));

		// Find matching descriptor
		//
		std::cout << "Number of entries: " << db.num_entries << std::endl;
		if (db.num_entries == 0) {
			std::cout << "No entries in block " << parent_block << std::endl;
			exit(1);
			break;
		}

		for (int i = 0; i < db.num_entries; i++) {
			/* std::cout<<db.descriptors[i].name<<std::endl; */
			/* std::cout << "Comparing " << db.descriptors[i].name << " with " << f->name << std::endl; */
			if (strcmp(db.descriptors[i].name, f->name.c_str()) == 0) {
				desc = db.descriptors[i];
				index = i;
				found = true;
				std::cout << "Found descriptor" << std::endl;
				break;
			}
		}

		if (!found && db.next_block > 0) {
			parent_block = db.next_block;
		}
	}

	assert(found);
	assert(desc.type == 0);

	auto current_number_of_blocks = (desc.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
	auto new_number_of_blocks = f->data_blocks.size();
	desc.size = f->size;

	std::cout << "Current number of blocks: " << current_number_of_blocks << std::endl;
	std::cout << "New number of blocks: " << new_number_of_blocks << std::endl;


	// Fill in the first 8 blocks
	while(current_number_of_blocks < sizeof(desc.blocks)/sizeof(uint32_t)) {
		if(current_number_of_blocks >= new_number_of_blocks) {
			break;
		}
		desc.blocks[current_number_of_blocks] = f->data_blocks[current_number_of_blocks];
		current_number_of_blocks++;
	}

	// If we need more blocks, we need to use a pointer block
	auto pointer_block_number = desc.pointer_block;
	while (current_number_of_blocks < new_number_of_blocks) {
		pointer_block pb = {};
		/* read_block(STATE->file, pointer_block_number, (char*)&pb, sizeof(pointer_block)); */
		bool dirty = false;

		// First time we need a pointer block
		if (pointer_block_number == 0){
			pointer_block_number = get_next_block();
			desc.pointer_block = pointer_block_number;
			dirty = true;
		} else {
			read_block(STATE->file, pointer_block_number, (char*)&pb, sizeof(pointer_block));
		}

		while (pb.num_pointers < sizeof(pb.blocks)/sizeof(uint32_t)) {
			pb.blocks[pb.num_pointers] = f->data_blocks[current_number_of_blocks];
			dirty = true;
			pb.num_pointers++;
			current_number_of_blocks++;
			if (current_number_of_blocks == new_number_of_blocks) {
				break;
			}
		}

		if (pb.next_pointer_block == 0 && current_number_of_blocks < new_number_of_blocks) {
			pb.next_pointer_block = get_next_block();
			dirty = true;

			pointer_block next_pb = {};
			write_block(STATE->file, pb.next_pointer_block, &next_pb);
			// For crash recovery, we always set the block before pointing to it.
		}

		if (dirty) {
			write_block(STATE->file, pointer_block_number, &pb);
		}

		pointer_block_number = pb.next_pointer_block;
	}


	// The other problem is if we have too many blocks
	if (current_number_of_blocks > new_number_of_blocks) {
		if (new_number_of_blocks <= sizeof(desc.blocks)/sizeof(uint32_t)) {
			free_pointer_blocks(desc.pointer_block, 0);
			desc.pointer_block = 0;
		} else {
			auto remaining_blocks = new_number_of_blocks - sizeof(desc.blocks)/sizeof(uint32_t);
			free_pointer_blocks(desc.pointer_block, remaining_blocks);
		}
		current_number_of_blocks = new_number_of_blocks;
	}

	// 2. Write the descriptor back
	db.descriptors[index] = desc;
	write_block(STATE->file, parent_block, &db);

	// 3. Mark deleted blocks as free
	for (auto block : f->deleted_blocks) {
		STATE->free_block_ranges.insert(std::make_pair(block, block));
	}
	f->deleted_blocks.clear();
}




inline std::vector<std::string> parse_path(const std::string& path, char delimiter = '/') {
    std::vector<std::string> chunks;
    std::stringstream stream(path);

    std::string chunk;

    while (std::getline(stream, chunk, delimiter)) {
        if (!chunk.empty()) { // Ignore empty chunks (e.g., for leading or double slashes)
            chunks.push_back(chunk);
        }
    }

    return chunks;
}


static int sffs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	std::cout << "sffs_readdir: " << path << std::endl;
	auto entries = parse_path(path);
	b_directory* root = STATE->root;


	for (auto entry : entries) {
		/* std::cout << "entry: " << entry << std::endl; */

		if (root->directories.find(entry) != root->directories.end()) {
			/* std::cout << "Found directory: " << entry << std::endl; */
			root = root->directories[entry];
		} else {
			std::cout << "Directory not found: " << entry << std::endl;
			return -ENOENT;
		}
	}

	describe(root);
	// Print size of directories and files
	/* std::cout << "Name: " << root->name << std::endl; */
	/* std::cout << "Directories: " << root->directories.size() << std::endl; */
	/* std::cout << "Files: " << root->files.size() << std::endl; */

	for (auto it = root->directories.begin(); it != root->directories.end(); it++) {
		filler(buf, it->first.c_str(), NULL, 0);
	}
	for (auto it = root->files.begin(); it != root->files.end(); it++) {
		filler(buf, it->first.c_str(), NULL, 0);
	}

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	return 0;
}



static int sffs_getattr(const char *path, struct stat *stbuf) {
	/* std::cout << "sffs_getattr: " << path << std::endl; */

	auto root = STATE->root;
	bool is_dir = true;

	b_file* f = nullptr;

	auto entries = parse_path(path);
	for (auto entry : entries) {
		if (root->directories.find(entry) != root->directories.end()) {
			root = root->directories[entry];
		} else if (root->files.find(entry) != root->files.end()) {
			is_dir = false;
			f = root->files[entry];
			break;
		} else {
			return -ENOENT;
		}
	}

	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0777;
		stbuf->st_nlink = 2;
	} else if (is_dir) {
		stbuf->st_mode = S_IFDIR | 0777;
		stbuf->st_nlink = 2;
	} else {
		stbuf->st_mode = S_IFREG | 0666;
		stbuf->st_nlink = 1;
		stbuf->st_size = f->size;
	}
	return 0;
}

static int sffs_mkdir(const char *path, mode_t mode) {
	/* std::cout << "sffs_mkdir: " << path << std::endl; */

	auto entries = parse_path(path);
	auto root = STATE->root;

	for (auto entry : entries) {
		if (root->directories.find(entry) != root->directories.end()) {
			root = root->directories[entry];
		} else {
			add_empty_entry(STATE->file, *root, entry, true);
		}
	}


	/* std::cout << "Directory created: " << path << std::endl; */
	/* std::cout << "Parent " << root->name << " has " <<  root->directories.size() << " directories and " << root->files.size() << " files." << std::endl; */


	return 0;
}


static int sffs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
	std::cout << "sffs_create: " << path << std::endl;

	auto entries = parse_path(path);
	auto root = STATE->root;

	for (auto entry : entries) {
		if (root->directories.find(entry) != root->directories.end()) {
			root = root->directories[entry];
		} else {
			add_empty_entry(STATE->file, *root, entry, false);
			break;
		}
	}

	fi->fh = STATE->available_file_handle++;
	STATE->open_files[fi->fh] = root->files[entries.back()];

	return 0;
}

static int sffs_open(const char *path, struct fuse_file_info *fi) {
	std::cout << "sffs_open: " << path << std::endl;

	auto entries = parse_path(path);
	auto root = STATE->root;

	for (auto entry : entries) {
		if (root->directories.find(entry) != root->directories.end()) {
			root = root->directories[entry];
		} else if (root->files.find(entry) != root->files.end()) {
			fi->fh = STATE->available_file_handle++;
			STATE->open_files[fi->fh] = root->files[entry];
			return 0;
		} else {
			return -ENOENT;
		}
	}

	return 0;
}

static int sffs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	std::cout << "sffs_write: " << path << " size: " << size << " offset: " << offset << std::endl;
	
	b_file *f = STATE->open_files[fi->fh];
	describe(f);

	auto first_block = offset / BLOCK_SIZE;
	auto last_block = (offset + size - 1) / BLOCK_SIZE;
	auto ptr = 0;

	auto existing_blocks = f->data_blocks.size();

	while (f->data_blocks.size()<= last_block) {
		f->data_blocks.push_back(get_next_block());
		/* std::cout << "Adding block " << f->data_blocks.back() << std::endl; */
		f->dirty_metadata = true;
	}
	if (f->size < offset + size) {
		/* std::cout << "Updating size from " << f->size << " to " << offset + size << std::endl; */
		f->size = offset + size;
		f->dirty_metadata = true;
	}

	auto block = first_block;
	while (size > 0) {
		auto block_number = f->data_blocks[block];

		auto start_offset = (block == first_block)? offset % BLOCK_SIZE : 0;

		data_block db = {};
		if ((start_offset > 0 || size < BLOCK_SIZE) && block < existing_blocks) {
			read_block(STATE->file, block_number, (char*)&db, sizeof(data_block));
		}


		auto bytes_to_write = std::min(static_cast<uint32_t>(size), static_cast<uint32_t>(BLOCK_SIZE - start_offset));

		memcpy(db.data + start_offset, buf + ptr, bytes_to_write);
		write_block(STATE->file, block_number, &db);

		ptr += bytes_to_write;
		size -= bytes_to_write;
		block++;
	}

	return ptr;
}


static int sffs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	std::cout << "sffs_read: " << path << " size: " << size << " offset: " << offset << std::endl;

	b_file *f = STATE->open_files[fi->fh];
	assert(f);
	describe(f);

	auto first_block = offset / BLOCK_SIZE;
	/* auto last_block = (offset + size - 1) / BLOCK_SIZE; */
	auto ptr = 0;

	size = std::min(static_cast<size_t>(f->size - offset), size);

	auto block = first_block;
	while (size > 0) {
		auto block_number = f->data_blocks[block];
		/* std::cout << "Reading " << size << " bytes from block " << block << std::endl; */

		auto start_offset = (block == first_block)? offset % BLOCK_SIZE : 0;

		data_block db = {};
		read_block(STATE->file, block_number, (char*)&db, sizeof(data_block));

		auto bytes_to_read = std::min(static_cast<uint32_t>(size), static_cast<uint32_t>(BLOCK_SIZE - start_offset));

		memcpy(buf + ptr, db.data + start_offset, bytes_to_read);

		ptr += bytes_to_read;
		size -= bytes_to_read;
		block++;
	}

	return ptr;
}


// TODO: Flush the backing file as well
static int sffs_flush(const char *path, struct fuse_file_info *fi) {
    std::cout << "sffs_flush: " << path << std::endl;

    b_file *f = STATE->open_files[fi->fh];
    if (!f) return -ENOENT;
	
	STATE->open_files.erase(fi->fh);

    if (f->dirty_metadata) {
        write_file_metadata(f); // Write metadata changes to disk
        f->dirty_metadata = false;
    }
    return 0;
}

static int sffs_release(const char *path, struct fuse_file_info *fi) {
    std::cout << "sffs_release: " << path << std::endl;
    return 0;
}





static void* sffs_init(struct fuse_conn_info *conn) {
	std::cout << "sffs_init" << std::endl;

	std::fstream file(STATE->file_name, std::ios::in | std::ios::out | std::ios::binary);

	if (!file.is_open()) {
		std::cout << "File not found: " << STATE->file_name << std::endl;
		std::fstream new_file(STATE->file_name, std::ios::out | std::ios::binary);
		directory_block root_block = {};
		write_block(new_file, 0, &root_block);
		new_file.close();

		file.open(STATE->file_name, std::ios::in | std::ios::out | std::ios::binary);
	}

	assert(file.good());

	std::set<uint32_t> visited;

	auto start = std::chrono::high_resolution_clock::now();
	b_directory* root = decode_directory(file, 0, visited);
	auto end = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
	std::cout << "Decoding took " << duration.count() << " microseconds" << std::endl;


	root->name = "/";
	STATE->root = root;

	describe(STATE->root);


	STATE->free_block_ranges.clear();
	int32_t prev_block = 0;
	for (auto v : visited) {
		if (v - prev_block > 1) {
			std::cout << "Gap between " << prev_block << " and " << v << std::endl;
			STATE->free_block_ranges.insert(std::make_pair(prev_block + 1, v));
		}
		prev_block = v;
	}
	STATE->free_block_ranges.insert(std::make_pair(prev_block + 1, MAX_BLOCKS));
	STATE->file = std::move(file);

	return STATE;
}


static int sffs_truncate(const char *path, off_t size) {
	std::cout << "sffs_truncate: " << path << " to " << size << std::endl;

	
	// 1. Find the file
	// 2. Update the size
	
	b_file* f = nullptr;

	
	auto entries = parse_path(path);
	auto root = STATE->root;
	for(auto entry : entries) {
		if (root->directories.find(entry) != root->directories.end()) {
			root = root->directories[entry];
		} else if (root->files.find(entry) != root->files.end()) {
			f = root->files[entry];
		} else {
			return -ENOENT;
		}
	}


	if (f->size == size) {
		// Nothing to do
		std::cout << "Size is already " << size << std::endl;
		return 0;
	}

	describe(f);

	
	if(f->size < size) {
		std::cout << "Expanding file from " << f->size << " to " << size << std::endl;
		assert(false && "Not implemented");
		return -1;
	} 
	
	std::cout << "Shrinking file from " << f->size << " to " << size << std::endl;
	auto current_number_of_blocks = (f->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
	auto new_number_of_blocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;

	for (int i = new_number_of_blocks; i < current_number_of_blocks; i++) {
		f->deleted_blocks.insert(f->data_blocks[i]);
	}
	f->size = size;

	describe(f);
	write_file_metadata(f);

	return 0;
}

static const struct fuse_operations sffs_oper = {
	.init = sffs_init,
	.readdir = sffs_readdir,
	.getattr = sffs_getattr,
	.mkdir = sffs_mkdir,
	.create = sffs_create,
	.open = sffs_open,
	.write = sffs_write,
	.read = sffs_read,
	.flush = sffs_flush,
	.release = sffs_release,
	.truncate = sffs_truncate,
};


long page_size() {
	long page_size = sysconf(_SC_PAGESIZE);
	std::cout << "Page size: " << page_size << " bytes" << std::endl;
	return page_size;
}


int main(int argc, char *argv[]) {
	std::cout <<"Block size: " << BLOCK_SIZE << " bytes" << std::endl;
	/* std::ios::sync_with_stdio(false); */
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);


	// parse options
	if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1){
		return 1;
	}
	
	sffs_state state;
	state.file_name = options.file_name;

	std::cout << "File: " << state.file_name << std::endl;

	return fuse_main(args.argc, args.argv, &sffs_oper, &state);
}
