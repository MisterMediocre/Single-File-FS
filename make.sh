#!/bin/bash

# Variables
CXX="g++"
VERSION="-std=c++17"
WALL="-Wall"
INCLUDE_PATH="$(brew --prefix)/Cellar/libssh/0.11.1/include"
LIB_PATH="$(brew --prefix)/Cellar/libssh/0.11.1/lib"
FUSE_FLAGS=$(pkg-config --cflags --libs fuse)
# LARGE_FILE_FLAGS="-D_FILE_OFFSET_BITS=64"
IGNORE_DEPRECATED_WARNINGS="-Wno-deprecated-declarations"
LIBS="-lssh"
SRC="sffs.cpp"
OUTPUT="sffs.o"
BLOCK_SIZE=$1



# Compile the program
echo "Compiling $SRC..."

# Construct the command
CMD="$CXX $VERSION $IGNORE_DEPRECATED_WARNINGS $WALL $SRC $FUSE_FLAGS $LARGE_FILE_FLAGS -I$INCLUDE_PATH -L$LIB_PATH $LIBS -o $OUTPUT -DBLOCK_SIZE=$BLOCK_SIZE"

# Print the command
echo "Running command: $CMD"

# Execute the command
$CMD


TO_RESET=$2
if [ "$TO_RESET" = "reset" ]; then
	echo "Resetting the target directory..."
	rm something
fi
umount target
./sffs.o --file_name=something -f -o rw target
