/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.
*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//The attribute packed means to not align these things
struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ;

typedef struct cs1550_root_directory cs1550_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
} ;


typedef struct cs1550_directory_entry cs1550_directory_entry;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE - sizeof(long))

struct cs1550_disk_block
{
	//The next disk block, if needed. This is the next pointer in the linked 
	//allocation list
	long nNextBlock;

	//And all the rest of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block cs1550_disk_block;



static int cs1550_find_dir_loc(char* dir){

  FILE *disk = fopen(".disk","rb");
  if(disk == NULL){
    printf("error opening .disk\n");
    return -1;
  }
  printf("opened .disk\n");

  cs1550_root_directory *root = malloc(sizeof(cs1550_root_directory));

  int read_ret;
  read_ret = fread((void*)root, sizeof(cs1550_root_directory), 1, disk);
  if(read_ret<=0){
    printf("error reading the root directory\n");
    return -1;
  }
  printf("read the root\n");
  int i;
  for(i=0; i<MAX_DIRS_IN_ROOT; i++){
    char *name = "";
    if(root->directories!=NULL)
      name = root->directories[i].dname;
    else
      continue;
    printf("we're looking at dname %s\n", name);
    if(strcmp(name,dir)==0){
      fclose(disk);
      printf("found dir: %s\n", dir);
      return i;
    }
  }
  fclose(disk);
  printf("did not find dir: %s", dir);
  return -ENOENT; //not found
}


static int cs1550_find_file_loc(int dir_loc, char * file, size_t * fsize, long* fblock){
  FILE * disk = fopen(".disk","rb");
  if(disk==NULL){
    printf("error opening .disk \n");
    return -1;
  }
  cs1550_root_directory *root;

  int read_ret;
  read_ret = fread((void*)root, sizeof(cs1550_root_directory), 1, disk);
  if(read_ret<=0){
    printf("error reading the root directory");
    return -1;
  }

  long block = root->directories[dir_loc].nStartBlock;

  fseek(disk, block, SEEK_SET);
  
  cs1550_directory_entry * entry;
  
  read_ret = fread((void*) entry, sizeof(cs1550_directory_entry), 1, disk);
  if(read_ret<=0){
    printf("error reading directory entry");
    return -1;
  }
  
  int i;
  for(i=0; i<MAX_FILES_IN_DIR; i++){
    char * name; 
    if(entry->files!=NULL){
      name = entry->files[i].fname;
      if(strcmp(name,file)==0){
        fclose(disk);
        fsize = &(entry->files[i].fsize);
        fblock = &(entry->files[i].nStartBlock);
        return i;
      }
    }
  }
  fclose(disk);
  return -ENOENT;

}


/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not. 
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int cs1550_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;

	memset(stbuf, 0, sizeof(struct stat));
  	
	
	char dirname[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];
 
	//set strings to empty
	memset(dirname,  0,MAX_FILENAME  + 1);
	memset(filename, 0,MAX_FILENAME  + 1);
	memset(extension,0,MAX_EXTENSION + 1);

	//is path the root dir?
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		return 0;
	} else { //it is not the root
		
		//let's seperate this thing
		sscanf(path,"/%[^/]/%[^.].%s",dirname,filename,extension);
		printf("we're looking for / %s / %s . %s \n", dirname, filename, extension);
		int dir_loc = cs1550_find_dir_loc(dirname);
		printf("cs1550_find_dir_loc returned %d\n", dir_loc);
                if(dir_loc<0) //dir does not exist
			return -ENOENT;
		//Check if name is subdirectory
		if(filename[0]=='\0'){ //no file name, and dir exists
			stbuf->st_mode  = S_IFDIR | 755;
			stbuf->st_nlink = 2;
			return 0;
		}
		else{ //we're looking for a file in dirname which is indexed at dir_loc
 			size_t fsize = 0;
			int file_loc = cs1550_find_file_loc(dir_loc, filename, &fsize, NULL);
			if(file_loc<0)
				return -ENOENT;
			stbuf->st_mode = S_IFREG | 0666;
			stbuf->st_nlink = 1;
			stbuf->st_size = fsize;
			return 0; 
		}	

	//Check if name is a regular file
	/*
		//regular file, probably want to be read and write
		stbuf->st_mode = S_IFREG | 0666; 
		stbuf->st_nlink = 1; //file links
		stbuf->st_size = 0; //file size - make sure you replace with real size!
		res = 0; // no error
	*/

		//Else return that path doesn't exist
		res = -ENOENT;
	}
	return res;
}

/* 
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	(void) offset;
	(void) fi;

	//This line assumes we have no subdirectories, need to change
	if (strcmp(path, "/") != 0)
	return -ENOENT;

	//the filler function allows us to add entries to the listing
	//read the fuse.h file for a description (in the ../include dir)
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	
	FILE * disk = fopen(".disk", "rb+");
        cs1550_root_directory  root;
        int read_ret = fread((void*) &root, sizeof(cs1550_root_directory), 1, disk);
        if(read_ret<=0){
          printf("error reading root directory\n");
          return -1;
        }
	
	int i;
	for(i = 0; i<MAX_DIRS_IN_ROOT; i++){
	  if(root.directories[i].dname[0]!=0){ //this dir exists!
	    printf("adding %s to readir\n", root.directories[i].dname);
	    filler(buf, root.directories[i].dname, NULL, 0);
	  }
	}
	
	/*
	//add the user stuff (subdirs or files)
	//the +1 skips the leading '/' on the filenames
	filler(buf, newpath + 1, NULL, 0);
	*/
	return 0;
}

/* 
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode)
{
	(void) path;
	(void) mode;
	//FILE * disk = fopen(".disk", "rb+");
//	if(disk == NULL){
//	  printf("error opening .disk\n");
//	  return -1;
//	}
//        cs1550_root_directory * root = NULL;
//	int read_ret = fread((void*)root, sizeof(cs1550_root_directory), 1,disk);
//        if(read_ret<=0){
//	  //fclose(disk);
//	  printf("error reading the root directory\n");
//	  return -1;
//	}


	char directory[MAX_FILENAME+1];
        char filename [MAX_FILENAME+1];
        char extension[MAX_EXTENSION+1];

        //set strings to empty
        memset(directory, 0,MAX_FILENAME  + 1);
        memset(filename,  0,MAX_FILENAME  + 1);
        memset(extension, 0,MAX_EXTENSION + 1);

	printf("mkdir path: %s\n", path);
	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
	if(directory == NULL){
	  printf("could not sscanf dir name, dir: %s\n", directory);
	  return -1;
	}
	printf("checking length of %s \n", directory);
	if(strlen(directory)>8||strlen(directory)<=0){
	  //fclose(disk);
	  printf("name too long (or short)\n");
	  return -ENAMETOOLONG;
	}
	printf("good length for directory name\n");
	int loc = cs1550_find_dir_loc(directory);
	if(loc>=0){ //it already exists
	  //fclose(disk);
	  printf("dir exists\n");
	  return -EEXIST;
	}
	printf("does not already exists\n");
	FILE * disk = fopen(".disk", "rb+");
	cs1550_root_directory  root;
	int read_ret = fread((void*) &root, sizeof(cs1550_root_directory), 1, disk);
	if(read_ret<=0){
	  printf("error reading root directory\n");
	  return -1;
	}
	if(root.nDirectories >=MAX_DIRS_IN_ROOT){
	  printf("too many dirs\n");
	  fclose(disk);
	  return -EPERM;
	}


	int i;
	for(i=0; i<MAX_DIRS_IN_ROOT; i++){

	  if(root.directories[i].dname[0]==0){ //empty dir

	    strcpy(root.directories[i].dname,directory);
	    root.directories[i].nStartBlock = -1; //TODO find a free block
	    break;
	  }

	}
	//TODO
	//make dir struct
	//seek to block
	//save dir to file at blockize * block bytes from start
	rewind(disk);
	fwrite((void*)&root, sizeof(cs1550_root_directory), 1, disk);
	fclose(disk);
	printf("wrote to and clsoed .disk\n");
	return 0;
}

/* 
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;
    return 0;
}

/* 
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
{
	(void) mode;
	(void) dev;
	return 0;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
    (void) path;

    return 0;
}

/* 
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//read in data
	//set size and return, or error

	size = 0;

	return size;
}

/* 
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char *path, const char *buf, size_t size, 
			  off_t offset, struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//write data
	//set size (should be same as input) and return, or error

	return size;
}

/******************************************************************************
 *
 *  DO NOT MODIFY ANYTHING BELOW THIS LINE
 *
 *****************************************************************************/

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or 
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int cs1550_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

    return 0;
}


/* 
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but 
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file 
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int cs1550_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
    .getattr	= cs1550_getattr,
    .readdir	= cs1550_readdir,
    .mkdir	= cs1550_mkdir,
	.rmdir = cs1550_rmdir,
    .read	= cs1550_read,
    .write	= cs1550_write,
	.mknod	= cs1550_mknod,
	.unlink = cs1550_unlink,
	.truncate = cs1550_truncate,
	.flush = cs1550_flush,
	.open	= cs1550_open,
};

//Don't change this.
int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &hello_oper, NULL);
}
