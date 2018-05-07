// This file is part of MD-REAL-IO.
//
// MD-REAL-IO is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// MD-REAL-IO is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with MD-REAL-IO.  If not, see <http://www.gnu.org/licenses/>.
//
// Author: Julian Kunkel
//
// This file is for a posix interface using direct IO.
// Author: Chuizheng Mneg
#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>

#include <plugins/md-posix-direct.h>

static int BLKSIZE = 512;

static char * dir = "out";
static int created_root_dir = 0;

static option_help options [] = {
  {'D', "root-dir", "Root directory", OPTION_OPTIONAL_ARGUMENT, 's', & dir},
  LAST_OPTION
};

static option_help * get_options(){
  return options;
}

static int initialize(){
  return MD_SUCCESS;
}

static int finalize(){
  return MD_SUCCESS;
}

static int prepare_global(){
  int ret = mkdir(dir, 0755);
  if(ret != 0){
    // check if the directory is empty
    DIR * d = opendir(dir);
    if( d == NULL ) goto err;
    struct dirent * entry;
    int i;
    for(i=0; i < 10; i++){
      entry = readdir(d);
      if(entry == NULL){
        break;
      }
    }
    closedir(d);

    if (i == 2){
      printf("WARN: Will use the existing (empty) directory\n");
      return MD_SUCCESS;
    }
    err:
    printf("ERROR: Could not create the directory: %s; error: %s\n", dir, strerror(errno));
    return MD_ERROR_UNKNOWN;
  }
  created_root_dir = 1;
  return MD_SUCCESS;
}

static int purge_global(){
  // delete index file
  char name[4096];
  sprintf(name, "%s/index", dir);
  unlink(name);

  if(created_root_dir){
    return rmdir(dir);
  }
  return MD_SUCCESS;
}

static int def_dset_name(char * out_name, int n, int d){
  sprintf(out_name, "%s/%d_%d", dir, n, d);
  return MD_SUCCESS;
}

static int def_obj_name(char * out_name, int n, int d, int i){
  sprintf(out_name, "%s/%d_%d/file-%d", dir, n, d, i);
  return MD_SUCCESS;
}

static int create_dset(char * filename){
  return mkdir(filename, 0755);
}

static int rm_dset(char * filename){
  return rmdir(filename);
}

static int write_obj(char * dirname, char * filename, char * buf, size_t file_size){
  int ret;
  int fd;
  fd = open(filename, O_CREAT | O_TRUNC | O_DIRECT | O_RDWR, 0644);
  if (fd == -1) return MD_ERROR_CREATE;

  size_t aligned_file_size = (file_size + BLKSIZE - 1) / BLKSIZE * BLKSIZE;
  char* tempbuf = malloc(aligned_file_size + BLKSIZE);
  char* alignedbuf = (char*)(((uintptr_t)tempbuf + BLKSIZE - 1) / BLKSIZE * BLKSIZE);
  memcpy(alignedbuf, buf, file_size);

  // ret = write(fd, buf, file_size);
  ret = write(fd, alignedbuf, aligned_file_size);
  ret = ( (size_t) ret == aligned_file_size) ? MD_SUCCESS: MD_ERROR_UNKNOWN;
  /* if (ret == MD_ERROR_UNKNOWN) {
      printf("error: %s\n", strerror(errno));
  } */
  close(fd);
  free(tempbuf);

  return ret;
}


static int read_obj(char * dirname, char * filename, char * buf, size_t file_size){
  int fd;
  int ret;
  fd = open(filename, O_DIRECT | O_RDWR);
  if (fd == -1) return MD_ERROR_FIND;

  size_t aligned_file_size = (file_size + BLKSIZE - 1) / BLKSIZE * BLKSIZE;
  char* tempbuf = malloc(aligned_file_size + BLKSIZE);
  char* alignedbuf = (char*)(((uintptr_t)tempbuf + BLKSIZE - 1) / BLKSIZE * BLKSIZE);

  // ret = read(fd, buf, file_size);
  ret = read(fd, alignedbuf, aligned_file_size);
  ret = ( (size_t) ret == aligned_file_size) ? MD_SUCCESS: MD_ERROR_UNKNOWN;
  memcpy(buf, alignedbuf, file_size);

  close(fd);
  free(tempbuf);
  return ret;
}

static int stat_obj(char * dirname, char * filename, size_t file_size){
  struct stat file_stats;
  int ret;
  ret = stat(filename, & file_stats);
  if ( ret != 0 ){
    return MD_ERROR_FIND;
  }
  return MD_SUCCESS;
}

static int delete_obj(char * dirname, char * filename){
  return unlink(filename);
}




struct md_plugin md_plugin_posix_direct = {
  "posix_direct",
  get_options,
  initialize,
  finalize,
  prepare_global,
  purge_global,

  def_dset_name,
  create_dset,
  rm_dset,

  def_obj_name,
  write_obj,
  read_obj,
  stat_obj,
  delete_obj
};
