#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <fuse_opt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

char rootdir[64];
char cache_dir[64];
char s3_dir[64];
char mount_dir[64];
unsigned long long block_size = 0;
unsigned long long cache_size = 0;

int cache_fetch(const char* path, uint32_t block_num, uint64_t offset,  char* buf, uint64_t len,ssize_t *bytes_read);

int cache_add(const char* path, uint32_t block_num, const char* buf, uint64_t len,ssize_t* bread);

int cache_init(char *cache_dir_input, uint64_t cache_size_input, unsigned long long cache_block_size, char* cache_img_input, char* cloud_dir_input);

void real_path(char* source, const char* relative_path) {
	// /home/fz49/
	strcpy(source, s3_dir);
	strcat(source, relative_path);
}

void real_path_root(char* source, const char* relative_path) {
	// /home/fz49/
	strcpy(source, rootdir);
	strcat(source, relative_path);
}

void real_path_cache(char* source, const char* relative_path) {
	// /home/fz49/
	strcpy(source, cache_dir);
	strcat(source, relative_path);
}

int rat_getattr(const char *path, struct stat *stbuf) {
	fprintf(stderr, "In rat_getattr function-----------------------\n");

	int retstat = 0;
	char real[64];
	real_path(real, path);
    fprintf(stderr, "current path: %s\n", real);
    retstat = lstat(real, stbuf);
    if (retstat == -1) {
    	char real2[64];
    	real_path_cache(real2, path);
    	fprintf(stderr, "current path: %s\n", real2);
    	retstat = lstat(real2, stbuf);
    	if (retstat == -1) {
    		return -errno;
    	}
    }
    return 0;
}

int rat_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	fprintf(stderr, "In rat_read function-----------------------\n");
	// int retval = 0;
	int bytes_read;
	char* s3_read_buf = NULL;
	fprintf(stderr, "size = %d\n", size);
	fprintf(stderr, "offset = %d\n", offset);
	// calculate the first_block number and last_block number
	uint32_t first_block = offset / block_size;
	uint32_t last_block = (offset + size) / block_size;

	fprintf(stderr, "first_block = %d\n", first_block);
	fprintf(stderr, "last_block = %d\n", last_block);
	size_t buf_offset = 0;

	for (uint32_t block = first_block; block <= last_block; block++) {
		off_t block_offset;
		size_t partial_block;
		if (block == first_block) {
			block_offset = offset - block * block_size;
		}
		else {
			block_offset = 0;
		}
		if (block == last_block) {
			partial_block = (offset + size) - (block * block_size) - block_offset;
		}
		else {
			partial_block = block_size - block_offset;
		}
		if (partial_block == 0) {
			continue;
		}
		// char real[64];
		// real_path(real, path);
		// struct stat real_stat;
		// if (stat(real, &real_stat) == -1) {
  //           fprintf(stderr, "stat on the s3 file failed\n");
  //           return -errno;
  //       }
        fprintf(stderr, "about to call cache_fetch............\n");
        ssize_t bread = 0;

        // 从cache里面找，找到返回true path block 
        fprintf(stderr, "partial block = %d\n", partial_block);
        int result = cache_fetch(path, block, block_offset, buf + buf_offset, partial_block, &bread);
        // cache里面没有，去s3找，找到加到cache
        if (result == -1) {
            fprintf(stderr, "read from cache failed\n");
        	// cache没找到，去s3找
        	fprintf(stderr, "need to read from s3_dir\n");
        	// fd 从open得到
            int fd = (int)fi->fh;
            s3_read_buf = (char*)malloc(block_size);
            int s3_read = pread(fd, s3_read_buf, block_size, block_size * block);
            if (s3_read == -1) {
            	fprintf(stderr, "read error form s3_dir\n");
            	return -1;
            }
            else {
            	fprintf(stderr, "read %lu bytes from s3_dir\n", (unsigned long)s3_read);
            	fprintf(stderr, "adding to cache.....about to call cache_add.......\n");
            	
            	ssize_t cache_add_bytes = 0;

            	if (cache_add(path, block, s3_read_buf, block_size, &cache_add_bytes) == -1) {
            		fprintf(stderr, "fail to add block to cache\n");
            	}

            	fprintf(stderr, "bytes added to cache = %zd\n", cache_add_bytes);

                memcpy(buf + buf_offset, s3_read_buf + block_offset, ((s3_read < block_size) ? s3_read : block_size));
                free(s3_read_buf);

                if (s3_read < block_size) {
                    fprintf(stderr, "read less than requested, %lu instead of %lu\n", (unsigned long)s3_read, (unsigned long)block_size);
                    bytes_read += s3_read;
                    fprintf(stderr, "bytes_read after =%lu\n", (unsigned long)bytes_read);       
                } 
                else {
                    fprintf(stderr, "%lu bytes for fuse buffer\n", (unsigned long) block_size);
                    bytes_read += block_size;
                    fprintf(stderr, "bytes_read=%lu\n", (unsigned long)bytes_read);
                }
            }
        }

        else {

        	fprintf(stderr, "file exists in cache_img\n");
        	// bread作为参数传到cache fetch
        	fprintf(stderr, "got %lu bytes from cache\n", (unsigned long)bread);
        	bytes_read += bread;
        	fprintf(stderr, "bytes_read = %lu\n", (unsigned long)bytes_read);
        	if (bread < block_size) {
                // must have read the end of file
                fprintf(stderr, "fewer than requested\n");
            }
        }
        buf_offset += block_size;
    }
    return bytes_read;
}

int rat_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	fprintf(stderr, "In rat_write function-----------------------\n");
	int bytes_write;
	off_t buf_offset = 0;
	uint32_t first_block = offset / block_size;
	uint32_t last_block = (offset + size) / block_size;
	// fprintf(stderr, "first_block = %d\n", first_block);
	// fprintf(stderr, "last_block = %d\n", last_block);
	for (uint32_t block = first_block; block <= last_block; block++) {
		size_t write_size;
		if (block == first_block) {
			write_size = ((block + 1) * block_size) - offset;
		}
		else if (block == last_block){
			write_size = size - buf_offset;
		}
		else {
            write_size = block_size;
		}
		if (write_size > size) {
            write_size = size;
		}
        if (write_size == 0) {
            continue;
        }
        fprintf(stderr, "writing block %lu, 0x%lx to 0x%lx\n", (unsigned long)block, 
        	(unsigned long)offset + buf_offset,
            (unsigned long)offset + buf_offset + write_size);
        // ssize_t temp_write = pwrite((int)fi->fh, buf + buf_offset, write_size, offset + buf_offset);
        ssize_t cache_add_bytes = 0;

        // write through cache, 就不往s3写了
        if (cache_add(path, block, buf + buf_offset, write_size, &cache_add_bytes) == -1) {
            fprintf(stderr, "fail to add block to cache\n");
        }

        fprintf(stderr, "bytes added to cache = %zd\n", cache_add_bytes);
        buf_offset += write_size;
    }
    return bytes_write;
}


int rat_open(const char *path, struct fuse_file_info *fi) {
	fprintf(stderr, "In rat_open function-----------------------\n");
    char real[64];
    real_path(real, path);
    int fd = open(real, O_RDWR | O_CREAT, S_IRWXG);
    if (fd == -1) {
       perror("Oprn: can not open\n");
    }
    fi->fh = fd;
    // 成功， 已经把fd送到fi->fh
    return 0;
}

int rat_opendir(const char *path, struct fuse_file_info *fi) {
    fprintf(stderr, "In rat_opendir function-----------------------\n");
    char real[64];
    real_path(real, path);
    DIR *dir = opendir(real);
    if (dir == NULL) {
        fprintf(stderr, "failed to open dir\n");
        return -1;
    }
    fi->fh = (uint64_t)(intptr_t)dir;
    return 0;
}

int rat_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	fprintf(stderr, "In rat_readdir function-----------------------\n");
    DIR *dir = (DIR*)(intptr_t)(fi->fh);
    struct dirent *entry = NULL;

    if (dir == NULL) {
        fprintf(stderr, "got null dir handle\n");
        return -1;
    }
    char real[64];
    real_path(real, path);

    while ((entry = readdir(dir)) != NULL) {
        filler(buf, entry->d_name, NULL, 0);
    }
    return 0;
}

int rat_releasedir(const char *path, struct fuse_file_info *fi) {
    fprintf(stderr, "In rat_releasedir function-----------------------\n");
    if (fi->fh != 0) {
        fprintf(stderr, "releasedir closing directory\n");
        DIR *dir = (DIR*)(intptr_t)(fi->fh);
        fi->fh = 0;
        closedir(dir);
    }
    return 0;
}

int rat_access(const char *path, int mask) {
    fprintf(stderr, "In rat_access function-----------------------\n");
    char real[64];
    real_path(real, path);
    int res = access(real, mask);
    if (res == -1) {
        return -errno;
    }
    return 0;
}

int rat_create(const char *path, mode_t mode, struct fuse_file_info *info) {
    fprintf(stderr, "In rat_create function-----------------------\n");
    char real[64];
    real_path(real, path);
    int retval = open(real, info->flags | O_CREAT | O_EXCL);
    // if (mkdir(real, 0700) == -1) {
    // 	perror("rat_create(): mkdir() failed");
    // 	return -errno;
    // }
    if (retval == -1) {
    	perror("error on rat_create");
    	return -errno;
    }

    info->fh = 10;
    int cmod = chmod(real, mode);
    if (cmod == -1) {
        fprintf(stderr, "failure on chmod in rat_create\n");
        return -errno;
    }
    return 0;
}

#ifdef HAVE_SYS_XATTR_H

int rat_setxattr(const char *path, const char *name, const char *value, size_t size, int flags) {
    fprintf(stderr, "In rat_setxattr function-----------------------\n");
    char real[64];
    real_path_cache(real, path);
    int ret = lsetxattr(real, name, value, size, flags);
    if (ret == -1) {
        perror("failure on setxattr");
        return -errno;
    }
    return 0;
}

int rat_getxattr(const char *path, const char *name, char *value, size_t size) {
    fprintf(stderr, "In rat_getxattr function-----------------------\n");
    char real[64];
    real_path_cache(real, path);
    int ret = getxattr(real, name, value, size);
    if (ret == -1) {
        perror("failure on getxattr");
        return -errno;
    }
    return 0;
}

#endif

int rat_flush(const char *path, struct fuse_file_info *fi) {
    fprintf(stderr, "In rat_flush function-----------------------\n");
    return 0;
}

int rat_utime(const char *path, struct utimbuf *ubuf) {
    fprintf(stderr, "In rat_utime function-----------------------\n");
    char real[64];
    real_path_cache(real, path);
    int ret = utime(real, ubuf);
    if (ret == -1) {
        perror("failure on utime");
        return -errno;
    }
    return 0;
}

int rat_truncate(const char *path, off_t newsize) {
    fprintf(stderr, "In rat_truncate function-----------------------\n");
    char real[64];
    real_path_cache(real, path);
    int ret = truncate(real, newsize);
    if (ret == -1) {
        perror("failure on truncate");
        return -errno;
    }
    return 0;    

}

static struct rat_operations : fuse_operations {
	rat_operations() {
		getattr = rat_getattr;
		read = rat_read;
		write = rat_write;
		open = rat_open;
		opendir = rat_opendir;
		readdir = rat_readdir;
        releasedir = rat_releasedir;
        access = rat_access;
        create = rat_create;
        utime = rat_utime;
        truncate = rat_truncate;
        
        flush = rat_flush;       
        #ifdef HAVE_SYS_XATTR_H
        getxattr = rat_getxattr;
        setxattr = rat_setxattr;
        #endif

	}
} rat_oper_init;


int main(int argc, char **argv) {
	// get current working directory: 
	struct rat_operations oper();

	getcwd(rootdir, sizeof(rootdir));
	strcat(rootdir, "/");

	char cache_img_input[64];
    const char *temp = "cache.img";
	real_path_root(cache_img_input, temp);
	real_path_root(cache_dir, argv[argc - 5]);
	real_path_root(s3_dir, argv[argc - 4]);
	real_path_root(mount_dir, argv[argc - 3]);

	argv[argc - 5] = cache_dir;
	argv[argc - 4] = s3_dir;
	argv[argc - 3] = mount_dir;

	//	char * cache_image = NULL;
	char *str_block_size = argv[argc - 1];
	char *endptr_block_size;
	block_size = strtoull(str_block_size, &endptr_block_size, 10);
	// fprintf(stderr, "convert char to unsigned long int = %llu\n", block_size);
	char *str_cache_size = argv[argc - 2];
	char *endptr_cache_size;
	cache_size = strtoull(str_cache_size, &endptr_cache_size, 10);

	fprintf(stderr, "cache_input_img = %s\n", cache_img_input);
	fprintf(stderr, "cache_dir = %s\n", argv[argc - 5]);
	fprintf(stderr, "s3_dir = %s\n", argv[argc - 4]);
	fprintf(stderr, "mount_dir = %s\n", argv[argc - 3]);
	fprintf(stderr, "cache_size = %s\n", argv[argc - 2]);
	fprintf(stderr, "block_size = %s\n", argv[argc - 1]);
	

	argv[argc - 5] = argv[argc - 3];
	argv[argc - 4] = NULL;
	argv[argc - 3] = NULL;
	argv[argc - 2] = NULL;
	argv[argc - 1] = NULL;
	argc -= 4;
	
	if (cache_init(cache_dir, cache_size, block_size, cache_img_input, s3_dir) == 0) {
		fprintf(stderr, "cache init success......\n");
	}

	// return fuse_main(argc, argv, &rat_operations, NULL);
	return fuse_main(argc, argv, &rat_oper_init, NULL);
}


