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
#include <signal.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <limits.h>
#include <unistd.h>

#define ERR(source) (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), kill(0, SIGKILL), exit(EXIT_FAILURE))

#ifndef PATH_MAX
#define PATH_MAX 1000
#endif
#define MAX_CHILDREN 128

typedef struct {
    pid_t pid;
    char target[PATH_MAX];
} child_info;

struct Watch {
    int wd;
    char *path;
};

struct WatchMap {
    struct Watch watch_map[128];
    int watch_count;
};


typedef struct {
    char src[PATH_MAX];
    char target[PATH_MAX];
    pid_t pid;
    int active;
} Backup;

void add_to_map(struct WatchMap *map, int wd, const char *path) {
    map->watch_map[map->watch_count].wd = wd;
    map->watch_map[map->watch_count].path = strdup(path);
    map->watch_count++;
}

struct Watch *find_watch(struct WatchMap *map, int wd) {
    for (int i = 0; i < map->watch_count; i++)
        if (map->watch_map[i].wd == wd)
            return &map->watch_map[i];
    return NULL;
}

void add_watch_recursive(int fd, struct WatchMap *map, const char *base_path) {
    uint32_t mask = IN_CREATE | IN_MODIFY | IN_DELETE |
                    IN_MOVED_FROM | IN_MOVED_TO |
                    IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED;

    int wd = inotify_add_watch(fd, base_path, mask);
    if (wd < 0)
        ERR("inotify_add_watch");

    add_to_map(map, wd, base_path);

    DIR *dir = opendir(base_path);
    if (!dir) return;

    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", base_path, e->d_name);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            add_watch_recursive(fd, map, path);
        }
    }
    closedir(dir);
}



volatile sig_atomic_t last_signal = 0;

void sethandler(void (*f)(int), int sigNo)
{
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = f;
    if (-1 == sigaction(sigNo, &act, NULL))
        ERR("sigaction");
}

void sig_handler(int sig) {last_signal = sig;}

void sigchld_handler(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

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

void restore_recursive(const char *src, const char* target, const char *src_root, const char *target_root){
    struct stat st;
    struct stat st_src;
    if(lstat(target, &st) == -1)
        ERR("lstat");
    if(lstat(src, &st_src) == -1 && errno != ENOENT)
        ERR("lstat");
    if (lstat(src, &st_src) == 0 &&
        (st_src.st_mode & S_IFMT) != (st.st_mode & S_IFMT)) {
        unlink(src);
        rmdir(src);
    }

    if(S_ISREG(st.st_mode)){
        if (lstat(src, &st_src) == -1 && errno == ENOENT) {
            copy_file(target, src);
            return;
        }

        int fd_target = open(target, O_RDONLY);
        int fd_source = open(src, O_RDONLY);
        if(fd_target == -1 || fd_source == -1){
            ERR("open");
        }
        char c_target, c_src;
        while(1){
            ssize_t len_target = read(fd_target, &c_target, 1);
            ssize_t len_source = read(fd_source, &c_src, 1);

            if(len_source == -1 || len_target == -1){
                ERR("read");
            }

            if(len_source == 0 && len_target == 0){
                break;
            }
            if(c_target != c_src || len_source != len_target){
                close(fd_target);
                close(fd_source);
                copy_file(target, src);
                return;
            }
        }
        // TOADD condtion or print when they identical
        close(fd_target);
        close(fd_source);
        return;
    }
    if(S_ISLNK(st.st_mode)){
        char linkbuf_src[PATH_MAX];
        char linkbuf_target[PATH_MAX];
        ssize_t len_src = readlink(src, linkbuf_src, sizeof(linkbuf_src)-1);
        ssize_t len_target = readlink(target, linkbuf_target, sizeof(linkbuf_target)-1);
        if (len_src == -1 && (errno == ENOENT || errno == EINVAL)){
            unlink(src);
            copy_symlink(target, src, target_root, src_root);
            return;
        }
        if (len_src == -1 || len_target == -1) 
            ERR("readlink");
        linkbuf_src[len_src] = '\0';
        linkbuf_target[len_target] = '\0';

        if(len_src != len_target || strcmp(linkbuf_src, linkbuf_target) != 0){
            unlink(src);
            copy_symlink(target, src, target_root, src_root);
        }
        return;
    }

    if(S_ISDIR(st.st_mode)){
        if(mkdir(src, 0777) == -1 && errno != EEXIST)
            ERR("mkdir");
        
        DIR *dir = opendir(target);
        if(dir == NULL)
            ERR("opendir");
        
        struct dirent *e;
        char src_path[PATH_MAX], target_path[PATH_MAX];

        while ((e = readdir(dir)) != NULL){
            if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;


            snprintf(src_path, sizeof(src_path), "%s/%s", src, e->d_name);
            snprintf(target_path, sizeof(target_path), "%s/%s", target, e->d_name);
            

            restore_recursive(src_path, target_path, src_root, target_root);
        }
        closedir(dir);

        DIR *dir_src = opendir(src);
        if (!dir_src)
            ERR("opendir");

        while ((e = readdir(dir_src)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;

            snprintf(src_path, sizeof(src_path), "%s/%s", src, e->d_name);
            snprintf(target_path, sizeof(target_path), "%s/%s", target, e->d_name);

            if (lstat(target_path, &st) == -1 && errno == ENOENT) {
                unlink(src);
                copy_symlink(target, src, target_root, src_root);
                return;
            }
        }
        closedir(dir_src);
    }
}

void child_work(char* real_source, char* real_target){
    sethandler(sig_handler, SIGINT);
    sethandler(sig_handler, SIGTERM);

    sigset_t sig_mask;
    sigfillset(&sig_mask);
    sigdelset(&sig_mask, SIGINT);
    sigdelset(&sig_mask, SIGTERM);
    sigdelset(&sig_mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &sig_mask, NULL);
    int fd = inotify_init();
    if (fd < 0) {
        ERR("inotify_init");}
    struct WatchMap map = {0};
    add_watch_recursive(fd, &map, real_source);


    char buf[65536];
    int to_exit = 0;

    while(!to_exit) {
        if (last_signal == SIGINT || last_signal == SIGTERM) {
            to_exit = 1;
            break;
        }
        ssize_t len = read(fd, buf, sizeof(buf));
        if (len < 0){ 
            if (errno == EINTR){
                continue;}
            ERR("read");}

        ssize_t i = 0;
        while (i < len) {
            struct inotify_event *event = (struct inotify_event *)&buf[i];
            struct Watch *w = find_watch(&map, event->wd);
            
            
            char source_path[PATH_MAX];
            char target_path[PATH_MAX];
            if (event->len > 0) {
                snprintf(source_path, sizeof(source_path), "%s/%s", w->path, event->name);
                snprintf(target_path, sizeof(target_path), "%s%s", real_target, source_path + strlen(real_source));
            } 
            else {
                strncpy(source_path, w->path, PATH_MAX);
                snprintf(target_path, sizeof(target_path), "%s%s", real_target, source_path + strlen(real_source));
            }

            if ((event->mask & IN_CREATE) || (event->mask & IN_MOVED_TO)) {

                if (event->len > 0) {
                    struct stat st1;
                    if (lstat(source_path, &st1) == -1) 
                        ERR("lstat");
                    if (S_ISDIR(st1.st_mode)) {
                        if (mkdir(target_path, st1.st_mode & 0777) == -1){
                            perror("mkdir(IN_CREATE)");}
                        add_watch_recursive(fd, &map, source_path);     
                        } 
                        
                    else if (S_ISREG(st1.st_mode)) 
                        copy_file(source_path, target_path);
                    else if (S_ISLNK(st1.st_mode)) 
                        copy_symlink(source_path, target_path, real_source, real_target);
                }
            }

            if ((event->mask & IN_DELETE) || (event->mask & IN_MOVED_FROM)){
                if(unlink(target_path) == -1)
                    if(remove(target_path) == -1)
                            if(rmdir(target_path) == -1)
                                    ERR("rmdir");}

            if ((event->mask & IN_MODIFY)){ 
                struct stat st2;

                if (lstat(source_path, &st2) == -1) 
                        ERR("lstat");
                else {
                    if(S_ISREG(st2.st_mode)){
                        copy_file(source_path, target_path);
                    }
                }
            }

            if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) {
                    to_exit = 1;
                    break;
            }


            i += sizeof(struct inotify_event) + event->len;
        }
    }
    close(fd);
    exit(0);
}

int parse_args(char *line, char *argv[])
{
    int argc = 0;
    char *p = line;

    while (*p && argc < 64) {
        while (*p == ' ' || *p == '\n')
            p++;

        if (*p == '\0')
            break;

        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"')
                p++;
            if (*p == '"') {
                *p = '\0';
                p++;
            }
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\n')
                p++;
            if (*p) {
                *p = '\0';
                p++;
            }
        }
    }
    return argc;
}


void exit_fun(child_info* children, int child_count){
    for (int i = 0; i < child_count; i++) {
                kill(children[i].pid, SIGTERM);
            }
            for (int i = 0; i < child_count; i++) {
                waitpid(children[i].pid, NULL, 0);
            }

            printf("Exiting program.\n");
            exit(0);
}

int main(){
    sethandler(sig_handler, SIGINT);
    sethandler(sig_handler, SIGTERM);

    sigset_t sig_mask;
    sigfillset(&sig_mask);
    sigdelset(&sig_mask, SIGINT);
    sigdelset(&sig_mask, SIGTERM);
    sigdelset(&sig_mask, SIGCHLD);
    sethandler(sigchld_handler, SIGCHLD);

    sigprocmask(SIG_BLOCK, &sig_mask, NULL);
    char cmd[4096];

    child_info children[MAX_CHILDREN];
    int child_count = 0;
    char curr_source[PATH_MAX];
    Backup backups[64];
    static int backup_count = 0;

    printf("Available commands:\n\t->add <souce path> <target path> - start monitoring and backing up source directory\n\t"
        "->end <source path> <target path> - stop monetring source directory\n\t"
        "->list - list currently monitoring directories\n\t"
        "->restore <source path> <target path> - restores files in souce dir form the last backup\n\t"
        "->exit - terminate all monitorings\n");
    while(1){
        if (last_signal == SIGINT || last_signal == SIGTERM) {
            
            break;
        }
        if (!fgets(cmd, sizeof(cmd), stdin)){
            if (errno == EINTR){
                continue;}
            break;}
        char* argv[64];
        int argc = parse_args(cmd, argv);
        //          EXIT
        if (strncmp(cmd, "exit", 4) == 0) {
            exit_fun(children, child_count);
        }
        //          ADD
        else if(argc >= 3 && strcmp(argv[0], "add") == 0){
            char real_target_check[PATH_MAX];
            int counter_of_dup_targets = 0;
            for(int i = 2; i < argc; i++){
                if (mkdir(argv[i], 0777) == -1 && errno != EEXIST) {
                    ERR("mkdir");
                }
                if(!realpath(argv[i], real_target_check)){
                    ERR("realpath");
                }
                for(int k = 0; k < child_count;k++){
                    if(strcmp(real_target_check, children[k].target) == 0){
                        fprintf(stderr, "CANT REPEAT TARGET!");
                        counter_of_dup_targets++;
                    }
                }
                for(int j = 0; j < argc; j++){
                    if(i != j && strcmp(argv[i], argv[j]) == 0){
                        counter_of_dup_targets++;
                    }
                }
            }
            if(counter_of_dup_targets != 0)
                break;
            char* source = argv[1];
            char real_source[PATH_MAX], real_target[PATH_MAX];

            //          INIT CHECK

            if (!realpath(source, real_source)) {
                fprintf(stderr, "Source does not exist!\n");
                continue;
            }
            struct stat st;
            if (stat(real_source, &st) == -1 || !S_ISDIR(st.st_mode)) {
                fprintf(stderr, "Source is not a directory!\n");
                continue;
            }
            strncpy(curr_source, real_source, PATH_MAX - 1);
            curr_source[PATH_MAX - 1] = '\0';

            for(int i = 2; i < argc;i++){
                char* target = argv[i];

                if (!realpath(target, real_target)) {
                    ERR("realpath");
                }

                if(strncmp(real_source, real_target, strlen(real_source)) == 0 && (real_target[strlen(real_source)] == '/' || real_target[strlen(real_source)] == '\0')){
                    fprintf(stderr, "target can NOT be in source!!");
                    continue;
                }
                
                if(!isDirectoryEmpty(real_target)){
                    fprintf(stderr, "target directory must be empty!\n");
                    continue;
                }

                //              INIT COPY

                printf("Starting backup...\n");
                copy_recursive(real_source, real_target, real_source, real_target);
                printf("Backup complete.\n");
                


                //           MONITORING INIT
                //https://gist.github.com/jaypeche/213df41e930860802cb5
                //          |
                //          |
                //          V
                pid_t pid = fork();
                if(pid == 0){
                    child_work(real_source, real_target);
                }
                else if(pid > 0){
                    children[child_count].pid = pid;
                    strncpy(children[child_count].target, real_target, PATH_MAX - 1);
                    children[child_count].target[PATH_MAX - 1] = '\0';
                    child_count++;
                    if(backup_count<64){
                        strcpy(backups[backup_count].src, real_source);
                        strcpy(backups[backup_count].target, real_target);
                        backups[backup_count].pid = pid;
                        backups[backup_count].active = 1;
                        backup_count++;
                    }
                    else{
                        fprintf(stderr, "too many backups at once");
                        continue;
                    }
                }
                else{
                    ERR("fork");
                }
            }
        }
        //          LIST
        else if (argc == 1 && strcmp(argv[0], "list") == 0) {
            int printed[64] = {0};
            for (int i = 0; i < backup_count; i++) {
                if (backups[i].active && !printed[i]) {
                    printf("SOURCE: %s\n", backups[i].src);
                    for(int j = 0; j < backup_count; j++){
                        if(strcmp(backups[i].src, backups[j].src) == 0 && backups[j].active == 1){
                            printf("\t-> %s\n", backups[j].target);
                            printed[j] = 1;
                        }
                    }
                }
            }
            continue;
        }
        //          END
        else if (argc >= 3 && strcmp(argv[0], "end") == 0) {
            char real_source[PATH_MAX];
            char real_target[PATH_MAX];
            if (!realpath(argv[1], real_source)) {
                fprintf(stderr, "Source does not exist\n");
                continue;
            }
            for (int i = 2; i < argc; i++) {
                if (!realpath(argv[i], real_target)) {
                    continue;
                }
                int exist = 0;
                for (int j = 0; j < backup_count; j++) {
                    if(backups[j].active == 1 && strcmp(backups[j].src, real_source) == 0 && strcmp(backups[j].target, real_target) == 0){
                        kill(backups[j].pid, SIGTERM);
                        waitpid(backups[j].pid, NULL, 0);

                        backups[j].active = 0;
                        exist = 1;

                        printf("backuping %s -> %s was stopped :(\n", backups[j].src, backups[j].target);
                    }
                }
                if(exist == 0){
                    fprintf(stderr, "No active backup for %s -> %s\n",real_source, real_target);
                }
            }
        }
        //          RESTORE
        else if(argc == 3 && strcmp(argv[0], "restore") == 0){
            char real_target[PATH_MAX];
            char real_src[PATH_MAX];
            if (!realpath(argv[1], real_src)) {
                ERR("realpath");
            }   
            if(!realpath(argv[2], real_target)){
                ERR("realpath");
            }
                struct stat st;
            if (stat(real_target, &st) == -1 || !S_ISDIR(st.st_mode)) {
                fprintf(stderr, "Target is not a directory!\n");
                continue;
            }

            printf("Restoring backup...\n");

            restore_recursive(real_src, real_target, real_src, real_target);

            printf("Restore complete.\n");
        }
        else{
            printf("Wrong command, please select one of the following:");
            printf("\n\t->add <souce path> <target path> - start monitoring and backing up source directory\n\t"
        "->end <source path> <target path> - stop monetring source directory\n\t"
        "->list - list currently monitoring directories\n\t"
        "->restore <source path> <target path> - restores files in souce dir form the last backup\n\t"
        "->exit - terminate all monitorings\n");
        }
    }    
    exit_fun(children, child_count);
}