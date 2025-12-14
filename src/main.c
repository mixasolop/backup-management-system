#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ERR(source) (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), kill(0, SIGKILL), exit(EXIT_FAILURE))

#ifndef PATH_MAX
#define PATH_MAX 1000
#endif
void usage(int argc, char* argv[])
{
    printf("%s n\n", argv[0]);
    printf("\t1 <= n <= 4 -- number of moneyboxes\n");
    exit(EXIT_FAILURE);
}

int isDirectoryEmpty(char *dirname) {
  int n = 0;
  struct dirent *d;
  DIR *dir = opendir(dirname);
  if (dir == NULL)
    return 1;
  while ((d = readdir(dir)) != NULL) {
    if(++n > 2)
      break;
  }
  closedir(dir);
  if (n <= 2)
    return 1;
  else
    return 0;
    //https://stackoverflow.com/questions/6383584/check-if-a-directory-is-empty-using-c-on-linux
}

void copy_file(const char* source, const char* target){
    int fd = open(source, O_RDONLY);
    if(fd == -1)
        {ERR("open");}
    int fd_target = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if(fd_target == -1)
        {ERR("open");}
    
    char buf[8192];
    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        if (write(fd_target, buf, r) != r)
            ERR("write");
    }
    if (r < 0) 
        ERR("read");

    close(fd);
    close(fd_target);
}


void copy_symlink(const char *src, const char *dst, const char *src_root, const char *dst_root){
    char linkbuf[PATH_MAX];
    ssize_t len = readlink(src, linkbuf, sizeof(linkbuf)-1);
    if (len < 0) 
        ERR("readlink");
    linkbuf[len] = '\0';

    char src_real[PATH_MAX], link_real[PATH_MAX];
    realpath(src_root, src_real);

    if (linkbuf[0] == '/' && realpath(linkbuf, link_real)) {
        if (strncmp(link_real, src_real, strlen(src_real)) == 0) {

            const char *sufix = link_real + strlen(src_real);
            char newlink[PATH_MAX];
            snprintf(newlink, sizeof(newlink), "%s%s", dst_root, sufix);

            if (symlink(newlink, dst) < 0)
                ERR("symlink rewrite");
            return;
        }
    }

    if (symlink(linkbuf, dst) < 0)
        ERR("symlink");
}

void copy_recursive(const char *src, const char *target, const char *src_root, const char *target_root) {
    struct stat st;
    if(lstat(src, &st) == -1)
        ERR("lstat");
    if(S_ISREG(st.st_mode)){
        copy_file(src, target);
        return;
    }
    if(S_ISLNK(st.st_mode)){
        copy_symlink(src, target, src_root, target_root);
        return;
    }

    if(S_ISDIR(st.st_mode)){
        if(mkdir(target, 0777) == -1 && errno != EEXIST)
            ERR("mkdir");
        
        DIR *dir = opendir(src);
        if(dir == NULL)
            ERR("opendir");
        
        struct dirent *e;
        char src_path[PATH_MAX], target_path[PATH_MAX];

        while ((e = readdir(dir)) != NULL){
            if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;


            snprintf(src_path, sizeof(src_path), "%s/%s", src, e->d_name);
            snprintf(target_path, sizeof(target_path), "%s/%s", target, e->d_name);
            

            copy_recursive(src_path, target_path, src_root, target_root);
        }
        closedir(dir);
    }
}




int main(int argc, char *argv[]){
    if(argc != 4){
        usage(argc,argv);
    }
    if(strcmp(argv[1], "add") == 0){
        char real_source[PATH_MAX], real_target[PATH_MAX];
        char* source = argv[2];
        char* target = argv[3];
        if (!realpath(source, real_source)) {
            fprintf(stderr, "Source does not exist!\n");
            return 1;
        }
        struct stat st;
        if (stat(real_source, &st) == -1 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "Source is not a directory!\n");
            return 1;
        }

        if (mkdir(target, 0777) == -1 && errno != EEXIST) {
            ERR("mkdir");
        }

        if (!realpath(target, real_target)) {
            ERR("realpath");
        }
        
        if(!isDirectoryEmpty(real_target)){
            fprintf(stderr, "target directory must be empty!\n");
            return 1;
        }

        printf("Starting backup...\n");
        copy_recursive(real_source, real_target, real_source, real_target);
        printf("Backup complete.\n");

    }
}