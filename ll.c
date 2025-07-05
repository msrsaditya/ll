#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <ctype.h>
#include <sys/wait.h>
#include <fnmatch.h>
#define MAX_PATH_LEN 1024
#define MAX_FILES 4096
#define C_RESET   "\x1b[0m"
#define C_HILIGHT "\x1b[7m" 
#define C_PS1_USER "\x1b[1;32m" 
#define C_PS1_SEP  "\x1b[1;37m" 
#define C_PS1_PATH "\x1b[1;34m" 
#define C_PS1_CWD  "\x1b[1;37m" 
#define C_FILE    "\x1b[0m"      
#define C_DIR     "\x1b[1;34m"   
#define C_LINK    "\x1b[1;36m"   
#define C_ORPHAN  "\x1b[1;31m"   
#define C_FIFO    "\x1b[33m"     
#define C_SOCK    "\x1b[1;35m"   
#define C_BLK     "\x1b[1;33m"   
#define C_CHR     "\x1b[1;33m"   
#define C_EXEC    "\x1b[1;32m"   
#define C_SUID    "\x1b[1;32m"   
#define C_SGID    "\x1b[1;32m"   
#define C_STICKY  "\x1b[1;34m"   
#define C_ARCHIVE "\x1b[1;31m"
#define C_IMAGE   "\x1b[1;35m"
#define C_AUDIO   "\x1b[0;36m"
#define C_DOC     "\x1b[1;34m" 
enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    BACKSPACE = 127
};
#define KEY_QUIT 'q'
#define KEY_UP 'k'
#define KEY_DOWN 'j'
#define KEY_OPEN 'l'
#define KEY_ENTER '\n'
#define KEY_BACK 'h'
#define KEY_GO_HOME '~'
#define KEY_TOGGLE_DOTFILES '.'
#define KEY_SHELL '!'
#define KEY_ESC 27
struct FileInfo {
    char *name;
    mode_t mode;
};
struct termios orig_termios; 
int screen_rows;
int screen_cols;
int show_dotfiles = 0; 
struct abuf {
    char *b;
    int len;
};
#define ABUF_INIT {NULL, 0}
void die(const char *s);
void disableRawMode();
void enableRawMode();
void spawnShell(const char* current_path);
void openFile(const char* file_path);
int natural_strcasecmp(const char *a, const char *b);
int compareFiles(const void *a, const void *b);
const char* getFileColor(const char *filename, mode_t mode);
const char* getFileIcon(const char *filename, mode_t mode);
void drawParentPane(struct abuf *ab, const char *path, const char* highlight_name, int x, int width, int height);
void drawPreviewPane(struct abuf *ab, const char *base_path, const char *filename, int start_col, int width, int height);
void listDir(const char *path);
void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}
void abFree(struct abuf *ab) {
    free(ab->b);
}
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    exit(1);
}
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
        die("tcsetattr");
    }
}
void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}
int readKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1);
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return KEY_ESC;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return KEY_ESC;
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
            }
        }
        return KEY_ESC;
    } else {
        return c;
    }
}
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return -1;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}
int natural_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *a_end, *b_end;
            long a_num = strtol(a, &a_end, 10);
            long b_num = strtol(b, &b_end, 10);
            if (a_num != b_num) {
                return a_num > b_num ? 1 : -1;
            }
            a = a_end;
            b = b_end;
        } else {
            int diff = tolower((unsigned char)*a) - tolower((unsigned char)*b);
            if (diff != 0) {
                return diff;
            }
            a++;
            b++;
        }
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int compareFiles(const void *a, const void *b) {
    const struct FileInfo *file_a = (const struct FileInfo *)a;
    const struct FileInfo *file_b = (const struct FileInfo *)b;
    int is_dir_a = S_ISDIR(file_a->mode);
    int is_dir_b = S_ISDIR(file_b->mode);
    if (is_dir_a && !is_dir_b) return -1;
    if (!is_dir_a && is_dir_b) return 1;
    return natural_strcasecmp(file_a->name, file_b->name);
}
void spawnShell(const char* current_path) {
    disableRawMode();
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    pid_t pid = fork();
    if (pid == -1) {
        die("fork");
    } else if (pid == 0) {
        if (chdir(current_path) != 0) {
            perror("chdir");
            exit(1);
        }
        char *shell = getenv("SHELL");
        if (shell == NULL) shell = "/bin/sh";
        execl(shell, shell, (char *)NULL);
        exit(127);
    } else {
        int status;
        waitpid(pid, &status, 0);
        enableRawMode();
    }
}
void openFile(const char* file_path) {
    disableRawMode();
    write(STDOUT_FILENO, "\x1b[?25h", 6);
    pid_t pid = fork();
    if (pid == -1) {
        die("fork");
    } else if (pid == 0) {
        #ifdef __linux__
        execlp("xdg-open", "xdg-open", file_path, (char *)NULL);
        #else
        execlp("open", "open", file_path, (char *)NULL);
        #endif
        perror("execlp failed");
        exit(127);
    } else {
        int status;
        waitpid(pid, &status, 0);
        enableRawMode();
        write(STDOUT_FILENO, "\x1b[?25l", 6);
    }
}
void runCommand() {
    char cmd[MAX_PATH_LEN] = {0};
    int cmd_len = 0;
    char buf[MAX_PATH_LEN + 10];
    snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[2K:", screen_rows);
    write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\x1b[?25h", 6);
    while(1) {
        int c = readKey();
        if (c == KEY_ENTER) {
            if (cmd_len > 0) break;
        } else if (c == KEY_ESC) {
            cmd_len = 0;
            break;
        } else if (c == BACKSPACE) {
            if (cmd_len > 0) cmd[--cmd_len] = '\0';
        } else if (isprint(c) && cmd_len < MAX_PATH_LEN -1) {
            cmd[cmd_len++] = c;
            cmd[cmd_len] = '\0';
        }
        snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[2K:%s", screen_rows, cmd);
        write(STDOUT_FILENO, buf, strlen(buf));
    }
    write(STDOUT_FILENO, "\x1b[?25l", 6);
    if (cmd_len > 0) {
        disableRawMode();
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        system(cmd);
        printf("\nPress any key to continue...");
        fflush(stdout);
        enableRawMode();
        readKey();
    }
}
const char* getFileColor(const char *filename, mode_t mode) {
    if (S_ISLNK(mode)) return C_LINK;
    if (S_ISDIR(mode)) return C_DIR;
    if (S_ISFIFO(mode)) return C_FIFO;
    if (S_ISSOCK(mode)) return C_SOCK;
    if (S_ISBLK(mode)) return C_BLK;
    if (S_ISCHR(mode)) return C_CHR;
    if (mode & S_IXUSR) return C_EXEC;
    const char *archives[] = {"*.tar", "*.tgz", "*.arc", "*.arj", "*.taz", "*.lha", "*.lz4", "*.lzh", "*.lzma", "*.tlz", "*.txz", "*.tzo", "*.t7z", "*.zip", "*.z", "*.dz", "*.gz", "*.lrz", "*.lz", "*.lzo", "*.xz", "*.zst", "*.tzst", "*.bz2", "*.bz", "*.tbz", "*.tbz2", "*.tz", "*.deb", "*.rpm", "*.jar", "*.war", "*.ear", "*.sar", "*.rar", "*.alz", "*.ace", "*.zoo", "*.cpio", "*.7z", "*.rz", "*.cab", "*.wim", "*.swm", "*.dwm", "*.esd", NULL};
    for (int i = 0; archives[i]; i++) {
        if (fnmatch(archives[i], filename, FNM_CASEFOLD) == 0) return C_ARCHIVE;
    }
    const char *images[] = {"*.jpg", "*.jpeg", "*.mjpg", "*.mjpeg", "*.gif", "*.bmp", "*.pbm", "*.pgm", "*.ppm", "*.tga", "*.xbm", "*.xpm", "*.tif", "*.tiff", "*.png", "*.svg", "*.svgz", "*.mng", "*.pcx", "*.mov", "*.mpg", "*.mpeg", "*.m2v", "*.mkv", "*.webm", "*.ogm", "*.mp4", "*.m4v", "*.mp4v", "*.vob", "*.qt", "*.nuv", "*.wmv", "*.asf", "*.rm", "*.rmvb", "*.flc", "*.avi", "*.fli", "*.flv", "*.gl", "*.dl", "*.xcf", "*.xwd", "*.yuv", "*.cgm", "*.emf", "*.ogv", "*.ogx", NULL};
    for (int i = 0; images[i]; i++) {
        if (fnmatch(images[i], filename, FNM_CASEFOLD) == 0) return C_IMAGE;
    }
    const char *audio[] = {"*.aac", "*.au", "*.flac", "*.m4a", "*.mid", "*.midi", "*.mka", "*.mp3", "*.mpc", "*.ogg", "*.ra", "*.wav", "*.oga", "*.opus", "*.spx", "*.xspf", NULL};
    for (int i = 0; audio[i]; i++) {
        if (fnmatch(audio[i], filename, FNM_CASEFOLD) == 0) return C_AUDIO;
    }
    const char *docs[] = {"*.pdf", "*.doc", "*.docx", "*.xls", "*.xlsx", "*.ppt", "*.pptx", "*.odt", "*.ods", "*.odp", "*.md", "*.txt", NULL};
    for (int i = 0; docs[i]; i++) {
        if (fnmatch(docs[i], filename, FNM_CASEFOLD) == 0) return C_DOC;
    }
    return C_FILE;
}
const char* getFileIcon(const char *filename, mode_t mode) {
    if (S_ISDIR(mode)) return "";
    if (S_ISLNK(mode)) return "";
    if (mode & S_IXUSR) return "";
    if (strcasecmp(filename, "makefile") == 0 || strcasecmp(filename, "cmakelists.txt") == 0) return "";
    if (strcasecmp(filename, "license") == 0 || strcasecmp(filename, "licence") == 0) return "";
    if (strcasecmp(filename, "dockerfile") == 0) return "";
    if (fnmatch("*docker-compose.y*ml", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.git*", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.tar", filename, FNM_CASEFOLD) == 0 || fnmatch("*.zip", filename, FNM_CASEFOLD) == 0 || fnmatch("*.rar", filename, FNM_CASEFOLD) == 0 || fnmatch("*.7z", filename, FNM_CASEFOLD) == 0 || fnmatch("*.gz", filename, FNM_CASEFOLD) == 0 || fnmatch("*.bz2", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.jpg", filename, FNM_CASEFOLD) == 0 || fnmatch("*.jpeg", filename, FNM_CASEFOLD) == 0 || fnmatch("*.png", filename, FNM_CASEFOLD) == 0 || fnmatch("*.gif", filename, FNM_CASEFOLD) == 0 || fnmatch("*.svg", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.mkv", filename, FNM_CASEFOLD) == 0 || fnmatch("*.mp4", filename, FNM_CASEFOLD) == 0 || fnmatch("*.mov", filename, FNM_CASEFOLD) == 0 || fnmatch("*.avi", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.mp3", filename, FNM_CASEFOLD) == 0 || fnmatch("*.flac", filename, FNM_CASEFOLD) == 0 || fnmatch("*.wav", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.pdf", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.md", filename, FNM_CASEFOLD) == 0 || fnmatch("*.markdown", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.html", filename, FNM_CASEFOLD) == 0 || fnmatch("*.htm", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.css", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.js", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.py", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.c", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.cpp", filename, FNM_CASEFOLD) == 0 || fnmatch("*.cxx", filename, FNM_CASEFOLD) == 0 || fnmatch("*.cc", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.h", filename, FNM_CASEFOLD) == 0 || fnmatch("*.hpp", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.sh", filename, FNM_CASEFOLD) == 0 || fnmatch("*.bash", filename, FNM_CASEFOLD) == 0 || fnmatch("*.zsh", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.json", filename, FNM_CASEFOLD) == 0) return "";
    if (fnmatch("*.yml", filename, FNM_CASEFOLD) == 0 || fnmatch("*.yaml", filename, FNM_CASEFOLD) == 0) return "";
    return "";
}
void drawParentPane(struct abuf *ab, const char *path, const char* highlight_name, int x, int width, int height) {
    DIR *d = opendir(path);
    if (!d) return;
    struct FileInfo entries[MAX_FILES];
    int entry_count = 0;
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL && entry_count < MAX_FILES) {
        if (!show_dotfiles && dir->d_name[0] == '.') continue;
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
        char item_path[MAX_PATH_LEN];
        snprintf(item_path, sizeof(item_path), "%s/%s", path, dir->d_name);
        struct stat item_st;
        if (lstat(item_path, &item_st) == 0) {
            entries[entry_count].name = strdup(dir->d_name);
            entries[entry_count].mode = item_st.st_mode;
            entry_count++;
        }
    }
    closedir(d);
    qsort(entries, entry_count, sizeof(struct FileInfo), compareFiles);
    int highlight_idx = -1;
    if (highlight_name) {
        for (int i = 0; i < entry_count; i++) {
            if (strcmp(entries[i].name, highlight_name) == 0) {
                highlight_idx = i;
                break;
            }
        }
    }
    int scroll_offset = 0;
    if (highlight_idx != -1 && highlight_idx >= height) {
        scroll_offset = highlight_idx - height + 1;
    }
    for (int i = 0; i < height && (i + scroll_offset) < entry_count; i++) {
        int idx = i + scroll_offset;
        const char *icon = getFileIcon(entries[idx].name, entries[idx].mode);
        const char *color = getFileColor(entries[idx].name, entries[idx].mode);
        char full_name[width + 4];
        snprintf(full_name, sizeof(full_name), "%s %s", icon, entries[idx].name);
        int display_width = width - 1;
        if (strlen(full_name) > display_width) {
            full_name[display_width - 1] = '~';
            full_name[display_width] = '\0';
        }
        char line_content[width + 2];
        snprintf(line_content, sizeof(line_content), " %s", full_name);
        char line[width + 64];
        char pos_buf[32];
        snprintf(pos_buf, sizeof(pos_buf), "\x1b[%d;%dH", i + 2, x);
        abAppend(ab, pos_buf, strlen(pos_buf));
        if (idx == highlight_idx) {
            snprintf(line, sizeof(line), "%s%s%-*s%s", color, C_HILIGHT, width, line_content, C_RESET);
        } else {
            snprintf(line, sizeof(line), "%s%-*s%s", color, width, line_content, C_RESET);
        }
        abAppend(ab, line, strlen(line));
    }
    for (int i = 0; i < entry_count; i++) free(entries[i].name);
}
void drawPreviewPane(struct abuf *ab, const char *base_path, const char *filename, int start_col, int width, int height) {
    if (!filename) return;
    char path[MAX_PATH_LEN];
    if (strcmp(base_path, "/") == 0) {
        snprintf(path, sizeof(path), "/%s", filename);
    } else {
        snprintf(path, sizeof(path), "%s/%s", base_path, filename);
    }
    struct stat path_stat;
    if (stat(path, &path_stat) != 0) return;
    if (S_ISDIR(path_stat.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return;
        struct FileInfo preview_entries[MAX_FILES];
        int entry_count = 0;
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL && entry_count < MAX_FILES) {
            if (!show_dotfiles && dir->d_name[0] == '.') continue;
            if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
            char item_path[MAX_PATH_LEN];
            snprintf(item_path, sizeof(item_path), "%s/%s", path, dir->d_name);
            struct stat item_st;
            if (stat(item_path, &item_st) == 0) {
                preview_entries[entry_count].name = strdup(dir->d_name);
                preview_entries[entry_count].mode = item_st.st_mode;
                entry_count++;
            }
        }
        closedir(d);
        qsort(preview_entries, entry_count, sizeof(struct FileInfo), compareFiles);
        if (entry_count == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "\x1b[2;%dH-- empty --", start_col + 2);
            abAppend(ab, buf, strlen(buf));
        } else {
            for (int i = 0; i < entry_count && i < height; i++) {
                const char *icon = getFileIcon(preview_entries[i].name, preview_entries[i].mode);
                const char *color = getFileColor(preview_entries[i].name, preview_entries[i].mode);
                char full_name[width + 4];
                snprintf(full_name, sizeof(full_name), "%s %s", icon, preview_entries[i].name);
                int display_width = width - 1;
                if (strlen(full_name) > display_width) {
                    full_name[display_width - 1] = '~';
                    full_name[display_width] = '\0';
                }
                char line_content[width + 2];
                snprintf(line_content, sizeof(line_content), " %s", full_name);
                char line_buf[width + 64];
                char pos_buf[32];
                snprintf(pos_buf, sizeof(pos_buf), "\x1b[%d;%dH", i + 2, start_col);
                abAppend(ab, pos_buf, strlen(pos_buf));
                if (i == 0) { 
                    snprintf(line_buf, sizeof(line_buf), "%s%s%-*s%s", color, C_HILIGHT, width, line_content, C_RESET);
                } else {
                    snprintf(line_buf, sizeof(line_buf), "%s%-*s%s", color, width, line_content, C_RESET);
                }
                abAppend(ab, line_buf, strlen(line_buf));
            }
        }
        for (int i = 0; i < entry_count; i++) free(preview_entries[i].name);
    } else if (S_ISREG(path_stat.st_mode)) {
        FILE *f = fopen(path, "r");
        if (!f) return;
        char line[2048];
        int y = 2;
        int is_binary = 0;
        int c;
        for(int i=0; i<512 && (c=fgetc(f)) != EOF; i++) {
            if (!isprint(c) && !isspace(c)) {
                is_binary = 1;
                break;
            }
        }
        rewind(f);
        if (is_binary) {
            char buf[64];
            snprintf(buf, sizeof(buf), "\x1b[2;%dH-- Binary File --", start_col + 2);
            abAppend(ab, buf, strlen(buf));
        } else {
            while (fgets(line, sizeof(line), f) && y <= height + 1) {
                if (strlen(line) > 0 && line[strlen(line) - 1] == '\n') {
                    line[strlen(line) - 1] = '\0';
                }
                char pos_buf[32], line_buf[2048];
                snprintf(pos_buf, sizeof(pos_buf), "\x1b[%d;%dH", y, start_col + 2);
                abAppend(ab, pos_buf, strlen(pos_buf));
                snprintf(line_buf, sizeof(line_buf), "%.*s", width - 2, line);
                abAppend(ab, line_buf, strlen(line_buf));
                y++;
            }
        }
        fclose(f);
    }
}
void listDir(const char *initial_path) {
    char current_path[MAX_PATH_LEN];
    strncpy(current_path, initial_path, MAX_PATH_LEN - 1);
    current_path[MAX_PATH_LEN - 1] = '\0';
    struct FileInfo files[MAX_FILES];
    int file_count = 0;
    int cursor_pos = 0;
    int scroll_offset = 0;
    char previous_dir_name[MAX_PATH_LEN] = "";
    while (1) {
        DIR *d = opendir(current_path);
        if (!d) {
            char temp_path[MAX_PATH_LEN];
            strncpy(temp_path, current_path, MAX_PATH_LEN);
            char *last_slash = strrchr(temp_path, '/');
            if (last_slash && last_slash != temp_path) *last_slash = '\0';
            else if (last_slash) strcpy(temp_path, "/");
            strncpy(current_path, temp_path, MAX_PATH_LEN);
            continue;
        }
        for (int i = 0; i < file_count; i++) free(files[i].name);
        file_count = 0;
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL && file_count < MAX_FILES) {
            if (!show_dotfiles && dir->d_name[0] == '.') continue;
            if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
            char path_buf[MAX_PATH_LEN];
            snprintf(path_buf, sizeof(path_buf), "%s/%s", current_path, dir->d_name);
            struct stat st;
            if (lstat(path_buf, &st) == 0) {
                files[file_count].name = strdup(dir->d_name);
                files[file_count].mode = st.st_mode;
                file_count++;
            }
        }
        closedir(d);
        qsort(files, file_count, sizeof(struct FileInfo), compareFiles);
        if (strlen(previous_dir_name) > 0) {
            int found = 0;
            for (int i = 0; i < file_count; i++) {
                if (strcmp(files[i].name, previous_dir_name) == 0) {
                    cursor_pos = i;
                    found = 1;
                    break;
                }
            }
            if (!found) cursor_pos = 0;
            previous_dir_name[0] = '\0';
        } else {
            if (cursor_pos >= file_count) {
                cursor_pos = file_count > 0 ? file_count - 1 : 0;
            }
        }
        if (file_count == 0) cursor_pos = 0;
        int redraw = 1;
        while(1) {
            if (redraw) {
                if (getWindowSize(&screen_rows, &screen_cols) == -1) die("getWindowSize");
                int left_pane_width = (int)(screen_cols * 0.172);
                int middle_pane_width = (int)(screen_cols * 0.328);
                int right_pane_width = screen_cols - left_pane_width - middle_pane_width;
                int left_pane_x = 1;
                int middle_pane_x = left_pane_width + 1;
                int right_pane_x = left_pane_width + middle_pane_width + 1;
                struct abuf ab = ABUF_INIT;
                abAppend(&ab, "\x1b[?25l", 6); 
                abAppend(&ab, "\x1b[2J", 4);   
                abAppend(&ab, "\x1b[H", 3);    
                char header[MAX_PATH_LEN * 2] = {0};
                char hostname[128];
                gethostname(hostname, sizeof(hostname));
                char* host_end = strchr(hostname, '.');
                if (host_end) *host_end = '\0';
                const char* user = getenv("USER");
                if (!user) user = "user";
                snprintf(header, sizeof(header), "%s%s@%s%s:%s",
                         C_PS1_USER, user, hostname, C_RESET, C_PS1_SEP);
                const char* home = getenv("HOME");
                char display_path[MAX_PATH_LEN * 2] = {0};
                char path_to_render[MAX_PATH_LEN];
                if (file_count > 0) {
                    if (strcmp(current_path, "/") == 0) {
                        snprintf(path_to_render, sizeof(path_to_render), "/%s", files[cursor_pos].name);
                    } else {
                        snprintf(path_to_render, sizeof(path_to_render), "%s/%s", current_path, files[cursor_pos].name);
                    }
                } else {
                    strncpy(path_to_render, current_path, sizeof(path_to_render));
                }
                if (home && strcmp(path_to_render, home) == 0) {
                    snprintf(display_path, sizeof(display_path), "%s~%s", C_PS1_CWD, C_RESET);
                } else if (strcmp(path_to_render, "/") == 0) {
                    snprintf(display_path, sizeof(display_path), "%s/%s", C_PS1_CWD, C_RESET);
                } else {
                    char temp_path[MAX_PATH_LEN];
                    strncpy(temp_path, path_to_render, sizeof(temp_path));
                    char *last_slash = strrchr(temp_path, '/');
                    char *name_part = temp_path;
                    char base_part_buf[MAX_PATH_LEN] = "";
                    if (last_slash) {
                        name_part = last_slash + 1;
                        size_t base_len = last_slash - temp_path;
                        if (base_len == 0) {
                            strcpy(base_part_buf, "/");
                        } else {
                            strncpy(base_part_buf, temp_path, base_len);
                            base_part_buf[base_len] = '\0';
                        }
                    }
                    char base_path_str[MAX_PATH_LEN];
                    if (strlen(base_part_buf) == 0) {
                        base_path_str[0] = '\0';
                    } else if (home && strcmp(base_part_buf, home) == 0) {
                        strcpy(base_path_str, "~");
                    } else if (home && strncmp(base_part_buf, home, strlen(home)) == 0) {
                        snprintf(base_path_str, sizeof(base_path_str), "~%s", base_part_buf + strlen(home));
                    } else {
                        strncpy(base_path_str, base_part_buf, sizeof(base_path_str));
                    }
                    if (strlen(base_path_str) == 0) {
                        snprintf(display_path, sizeof(display_path), "%s%s%s", C_PS1_CWD, name_part, C_RESET);
                    } else {
                        snprintf(display_path, sizeof(display_path), "%s%s%s%s%s%s",
                                 C_PS1_PATH, base_path_str,
                                 (strcmp(base_path_str, "/") == 0) ? "" : "/",
                                 C_PS1_CWD, name_part, C_RESET);
                    }
                }
                strncat(header, display_path, sizeof(header) - strlen(header) - 1);
                abAppend(&ab, header, strlen(header));
                abAppend(&ab, "\r\n", 2);
                char parent_path[MAX_PATH_LEN];
                char current_dir_name[MAX_PATH_LEN] = "";
                strncpy(parent_path, current_path, MAX_PATH_LEN);
                char *last_slash_parent = strrchr(parent_path, '/');
                if (last_slash_parent) {
                    strncpy(current_dir_name, last_slash_parent + 1, MAX_PATH_LEN);
                    if (parent_path == last_slash_parent && strlen(parent_path) > 1) {
                         *(last_slash_parent + 1) = '\0';
                    } else if (parent_path != last_slash_parent) {
                        *last_slash_parent = '\0';
                    }
                }
                if (strlen(parent_path) == 0) strcpy(parent_path, "/");
                drawParentPane(&ab, parent_path, current_dir_name, left_pane_x, left_pane_width, screen_rows - 2);
                if (cursor_pos < scroll_offset) scroll_offset = cursor_pos;
                if (cursor_pos >= scroll_offset + screen_rows - 2) {
                    scroll_offset = cursor_pos - (screen_rows - 2) + 1;
                }
                if (file_count == 0) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "\x1b[2;%dH-- empty --", middle_pane_x + 2);
                    abAppend(&ab, buf, strlen(buf));
                } else {
                    for (int i = 0; i < screen_rows - 2 && (i + scroll_offset) < file_count; i++) {
                        int idx = i + scroll_offset;
                        const char *icon = getFileIcon(files[idx].name, files[idx].mode);
                        const char *color = getFileColor(files[idx].name, files[idx].mode);
                        char full_name[middle_pane_width + 4];
                        snprintf(full_name, sizeof(full_name), "%s %s", icon, files[idx].name);
                        int display_width = middle_pane_width - 1;
                        if (strlen(full_name) > display_width) {
                            full_name[display_width - 1] = '~';
                            full_name[display_width] = '\0';
                        }
                        char line_content[middle_pane_width + 2];
                        snprintf(line_content, sizeof(line_content), " %s", full_name);
                        char line[middle_pane_width + 64];
                        char pos_buf[32];
                        snprintf(pos_buf, sizeof(pos_buf), "\x1b[%d;%dH", i + 2, middle_pane_x);
                        abAppend(&ab, pos_buf, strlen(pos_buf));
                        if (idx == cursor_pos) {
                            snprintf(line, sizeof(line), "%s%s%-*s%s", color, C_HILIGHT, middle_pane_width, line_content, C_RESET);
                        } else {
                            snprintf(line, sizeof(line), "%s%-*s%s", color, middle_pane_width, line_content, C_RESET);
                        }
                        abAppend(&ab, line, strlen(line));
                    }
                }
                if (file_count > 0) {
                    drawPreviewPane(&ab, current_path, files[cursor_pos].name, right_pane_x, right_pane_width, screen_rows - 2);
                }
                write(STDOUT_FILENO, ab.b, ab.len);
                abFree(&ab);
                redraw = 0;
            }
            int c = readKey();
            switch (c) {
                case KEY_QUIT:
                    write(STDOUT_FILENO, "\x1b[2J", 4);
                    write(STDOUT_FILENO, "\x1b[H", 3);
                    write(STDOUT_FILENO, "\x1b[?25h", 6);
                    for (int i = 0; i < file_count; i++) free(files[i].name);
                    exit(0);
                case KEY_UP: case ARROW_UP:
                    if (cursor_pos > 0) { cursor_pos--; redraw = 1; }
                    break;
                case KEY_DOWN: case ARROW_DOWN:
                    if (cursor_pos < file_count - 1) { cursor_pos++; redraw = 1; }
                    break;
                case KEY_SHELL:
                    spawnShell(current_path);
                    redraw = 1;
                    break;
                case KEY_ENTER:
                    runCommand();
                    redraw = 1;
                    break;
                case KEY_TOGGLE_DOTFILES:
                    show_dotfiles = !show_dotfiles;
                    cursor_pos = 0; scroll_offset = 0;
                    previous_dir_name[0] = '\0';
                    goto next_dir;
                case KEY_BACK: case ARROW_LEFT: {
                    char *last_slash = strrchr(current_path, '/');
                    if (last_slash && last_slash != current_path) {
                        strcpy(previous_dir_name, last_slash + 1);
                        *last_slash = '\0';
                        scroll_offset = 0;
                        goto next_dir;
                    } else if (last_slash && last_slash == current_path && strlen(current_path) > 1) {
                        strcpy(previous_dir_name, last_slash + 1);
                        strcpy(current_path, "/");
                        scroll_offset = 0;
                        goto next_dir;
                    }
                    break;
                }
                case KEY_GO_HOME: {
                    const char* home_dir = getenv("HOME");
                    if (home_dir) {
                        strncpy(current_path, home_dir, MAX_PATH_LEN -1);
                        cursor_pos = 0; scroll_offset = 0;
                        previous_dir_name[0] = '\0';
                        goto next_dir;
                    }
                    break;
                }
                case KEY_OPEN: case ARROW_RIGHT: {
                    if (file_count > 0) {
                        char new_path[MAX_PATH_LEN];
                        if (strcmp(current_path, "/") == 0) {
                           snprintf(new_path, sizeof(new_path), "/%s", files[cursor_pos].name);
                        } else {
                           snprintf(new_path, sizeof(new_path), "%s/%s", current_path, files[cursor_pos].name);
                        }
                        if (S_ISDIR(files[cursor_pos].mode)) {
                            strncpy(current_path, new_path, MAX_PATH_LEN - 1);
                            cursor_pos = 0; scroll_offset = 0;
                            previous_dir_name[0] = '\0';
                            goto next_dir;
                        } else if (S_ISREG(files[cursor_pos].mode)) {
                            openFile(new_path);
                            redraw = 1;
                        }
                    }
                    break;
                }
            }
        }
        next_dir:;
    }
}
int main(int argc, char *argv[]) {
    char initial_path[MAX_PATH_LEN];
    if (argc > 1) {
        realpath(argv[1], initial_path);
    } else {
        if (getcwd(initial_path, sizeof(initial_path)) == NULL) die("getcwd");
    }
    enableRawMode();
    listDir(initial_path);
    disableRawMode();
    write(STDOUT_FILENO, "\x1b[?25h", 6); 
    return 0;
}
