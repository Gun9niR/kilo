/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include<ctype.h>
#include<errno.h>
#include<fcntl.h>
#include<stdarg.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/ioctl.h>
#include<sys/types.h>
#include<termios.h>
#include<time.h>
#include<unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3
#define CTRL_KEY(k) ((k) & 0x1f)  //a bit mask that sets bits 5 and 6 bits of the character to 0，which is exactly how CTRL works
enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** data ***/

typedef struct erow {
    int size;
    int rsize;
    char *chars;  //raw text
    char *render;  //rendered text
}erow;

struct editor_config {
    int cx, cy;   //cursor position in the chars field, starting at 0
    int rx;       //cursor position in the render field, starting at 0
    int rowoff;   //row offset for vertical scrolling
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_terminos;
};

struct editor_config E;


/*** prototypes ***/

void editor_set_status_message(const char *fmt, ...);
void editor_move_cursor(int key);
void editor_refresh_screen();
char *editor_prompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    //perror() looks at the global errno variable and prints a descriptive error message for it. 
    //It also prints the string given to it before it prints the error message, 
    //which is meant to provide context about what part of your code caused the error.
    perror(s);
    exit(1);
}

void disable_raw_mode()
{
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_terminos) == -1)
        die("tcsetattr");
}

void enable_raw_mode()
{
    if(tcgetattr(STDIN_FILENO, &E.orig_terminos) == -1) die("tcgetattr");
    atexit(disable_raw_mode); //register the function to be called when program exits

    struct termios raw = E.orig_terminos;
    //switch off a couple flags by making the bit on the corresponding position 0
    //iflag, oflag, cflag, lflag are unsigned int
    //BRKINT: switch off break condition which terminates the program
    //ICRNL: switch off carriage returns being translated into newlines
    //INPCK: switch off parity checking (most likely obsolete)
    //ISTRIP: disable the 8th bit of each input byte being set to 0
    //IXON: switch off software flow control
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    //OPOST: switch off \n being translated into \r\n
    raw.c_oflag &= ~(OPOST);

    //CS8: set the character size to 8 bits per byte
    raw.c_cflag &= (CS8);

    //ECHO: stop printing the typed word in the terminal
    //ICANON: switch from line-by-line input to letter-by-letter
    //IEXTEN: switch off literal input (such as Ctrl-C being intputted as 3)
    //ISIG: switch off Ctr-C(terminate) and Ctr-Z(suspend) signal
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    //c_cc an array of unsigned char, size 32
    //VMIN: the minimum number of input bytes needed before read() can return
    //VMAX: the maximum amount of time to wait before read() returns, unit: 0.1s.
    //If read() times out, it returns 0.
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) ==-1) die("tcsetattr");
}

int editor_read_key() 
{
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if(nread == -1 && errno != EAGAIN) die("read");
    }

    if(c == '\x1b') {
        char seq[3];

        //if read() times out, will return <esc>
        if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if(seq[0] == '[') {
            if(seq[1] >= '0' && seq[1] <= '9') {
                if(read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if(seq[2] == '~') {
                    switch(seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch(seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if(seq[0] == '0') {
            switch(seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int get_cursor_position(int *rows, int *cols)
{
    char buf[32];
    unsigned int i=0;
    //"6n" inquires the cursor position
    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while(i < sizeof(buf) -1) {
        if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if(buf[i] == 'R') break;
        i++;
    }
    buf[i]='\0';

    if(buf[0] != '\x1b' || buf[1] != '[') return -1;
    if(sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int get_window_size(int *rows, int *cols)
{
    struct winsize ws;

    //'C' moves the cursor to the right
    //'B' moves the cursor down. Both commands check the bound
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        editor_read_key();
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

int editor_row_cx_to_rx(erow *row, int cx)
{
    int rx = 0;
    int j;
    for(j = 0; j < cx; j++) {
        if(row->chars[j] == '\t')
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

int editor_row_rx_to_cx(erow *row, int rx)
{
    int cur_rx = 0;
    int cx;
    for(cx = 0; cx < row->size; cx++) {
        if(row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        cur_rx++;
        if(cur_rx > rx) return cx;
    }
    return cx;
}

void editor_update_row(erow *row)
{
    //update rsize and render field
    int tabs = 0;
    int j = 0;
    for(j = 0; j < row->size; j++)
        if(row->chars[j] == '\t') tabs++;
    
    free(row->render); //it's Ok to free a NULL pointer
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1); //row->size already counts 1 for each tab

    int idx = 0;
    for(j = 0; j <row->size; j++) {
        if(row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while(idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else 
            row->render[idx++] = row->chars[j];
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editor_insert_row(int at, char *s, size_t len)
{
    if(at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editor_update_row(&E.row[at]);
    E.numrows++;
    E.dirty++;
}

void editor_free_row(erow *row)
{
    free(row->chars);
    free(row->render);
}

void editor_del_row(int at)
{
    if(at < 0 || at >= E.numrows) return;
    editor_free_row(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editor_row_insert_char(erow *row, int at, int c)
{
    if(at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2); //one for the new character, one for \0
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editor_update_row(row);
    E.dirty++;
}

void editor_row_append_string(erow *row, char *s, size_t len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
    E.dirty++;
}

void editor_row_del_char(erow *row, int at)
{
    if(at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
    E.dirty++;
}

/*** editor operations ***/

void editor_insert_char(int c)
{
    if(E.cy == E.numrows) {
        editor_insert_row(E.numrows, "", 0);
    }
    editor_row_insert_char(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editor_del_char()
{
    //this function is like backspace
    if(E.cy == E.numrows) return;
    if(E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if(E.cx > 0) {
        editor_row_del_char(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editor_row_append_string(&E.row[E.cy - 1], row->chars, row->size);
        editor_del_row(E.cy);
        E.cy--;
    }
}

void editor_insert_new_line()
{
    if(E.cx == 0) {
        editor_insert_row(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editor_insert_row(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editor_update_row(row);
    }
    E.cy++;
    E.cx = 0;
}

/*** file i/o ***/

char *editor_rows_to_string(int *buflen) 
{
    int totlen = 0;
    int j;
    for(j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;  // /r/n is stripped off when reading from file
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p= buf;
    for(j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editor_open(char *filename) 
{
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char *line = NULL;
    //size_t: unsigned int(32)/unsigned long(64)
    //ssize_t: int(32)/long (64)
    size_t linecap = 0;
    ssize_t linelen;
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        //strip off return carriage
        while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editor_insert_row(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editor_save() 
{
    if(E.filename == NULL) {
        E.filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
        if(E.filename == NULL) {
            editor_set_status_message("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editor_rows_to_string(&len);
    
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if(fd != -1) {
        if(ftruncate(fd, len) != -1)  //set file size to specific length
            if(write(fd, buf, len) == len) {
                editor_set_status_message("%d bytes written to disk", len);
                E.dirty = 0;
                close(fd);
                free(buf);
                return;
            }
        close(fd);
    }
    
    free(buf);
    editor_set_status_message("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editor_find_callback(char *query, int key)
{
    static int y_to_start = 0;
    static int x_to_start = 0;
    static int direction = 1;

    int len = strlen(query);
    int switch_direction = 0;

    if(key == '\r' || key == '\x1b' || !len) {
        y_to_start = E.cy;
        x_to_start = E.cx;
        direction = 1;
        return;
    } else if(key == ARROW_RIGHT || key == ARROW_DOWN) {
        if(direction == -1)
            switch_direction = 1;
        direction = 1;
    } else if(key == ARROW_LEFT || key == ARROW_UP) {
        if(direction == 1)
            switch_direction = 1;
        direction = -1;
    } else {
        y_to_start = E.cy;
        x_to_start = E.cx;
        direction = 1;
    }

    if(y_to_start == 0) {
        direction = 1;
    }

    int current_y = y_to_start;
    int lines_visited = 0;
    while(lines_visited < E.numrows) {
        erow *row = &E.row[current_y];
        char *match = NULL;
        if(direction == 1) {
            if(switch_direction) {
                x_to_start += len << 1;
                switch_direction = 0;
            }
            if(x_to_start >= row->rsize || !(match = strstr(row->render + x_to_start, query))) {
                lines_visited++;
                current_y += direction;
                if(current_y == -1) current_y = E.numrows - 1;
                else if(current_y == E.numrows) current_y = 0;
                x_to_start = 0;
                continue;
            }
        } else {
            if(switch_direction) {
                x_to_start -= len << 1;
                switch_direction = 0;
            }
            while(x_to_start >= 0 && strncmp(row->render + x_to_start, query, len))
                x_to_start--;
            if(x_to_start < 0) {
                lines_visited++;
                current_y += direction;
                if(current_y == -1) current_y = E.numrows - 1;
                else if(current_y == E.numrows) current_y = 0;
                x_to_start = E.row[current_y].rsize - len;
                continue;
            }
        }

        y_to_start = current_y;
        x_to_start = direction == 1 ? match - row->render + len : x_to_start - len;
        E.cy = current_y;
        E.cx = direction == 1 ? editor_row_rx_to_cx(row, match - row->render) : editor_row_rx_to_cx(row, x_to_start + len);
        return;
    }
}

void editor_find()
{
    char *query = editor_prompt("Search: %s (ESC/Arrows/Enter)", editor_find_callback);
    
    if(query)
        free(query);
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}  //constructor for abuf type

void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);

    if(new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/*** output ***/

void editor_scroll()
{
    E.rx = 0;
    if(E.cy < E.numrows)
        E.rx = editor_row_cx_to_rx(&E.row[E.cy], E.cx);

    if(E.cy < E.rowoff) //scroll upwards
        E.rowoff = E.cy; 
    if(E.cy >= E.rowoff + E.screenrows) //scroll downwards
        E.rowoff = E.cy - E.screenrows + 1;  //cursor is at the bottom
    if(E.rx < E.coloff)
        E.rowoff = E.rx;
    if(E.rx >= E.coloff + E.screencols)
        E.coloff = E.rx - E.screencols + 1;    
}

void editor_draw_rows(struct abuf *ab)
{
    int y;
    for(y = 0; y < E.screenrows; y++) {
        //file row points to the line in the file
        //while y points to the line on the screen
        int filerow = y + E.rowoff;

        if(filerow >= E.numrows) {
            if(E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
                if(welcomelen > E.screencols) welcomelen = E.screencols;
                //center the welcome message
                int padding = (E.screencols - welcomelen) / 2;
                if(padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while(padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }
            else
                abAppend(ab, "~" , 1);
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if(len < 0) len = 0;
            if(len > E.screencols) len = E.screencols;

#ifdef _LINE_NUM
            char linenum[32];
            snprintf(linenum, sizeof(linenum), "%d ", filerow + 1);
            abAppend(ab, linenum, strlen(linenum));
#endif

            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }
        //clear each line as we redraw them instead of clearing the entire page
        abAppend(ab, "\x1b[K", 3); 

        abAppend(ab, "\r\n", 2);
         
    }
}

void editor_draw_status_bar(struct abuf *ab)
{
    //m causes the text printed after it t be printed with various attributes
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No name]", E.numrows, E.dirty ? "(modified)" : "");
    if(len > E.screencols) len = E.screencols;
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
    abAppend(ab, status, len); 
    while(len < E.screencols) {
        if(E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab,"\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if(msglen > E.screencols) msglen = E.screencols;
    if(msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editor_refresh_screen()
{
    editor_scroll();

    struct abuf ab = ABUF_INIT; //use buffer so that we only need to do one write(), avoiding flickering

    //"?25l" hides the cursor when refreshing the screen to prevent flickering
    abAppend(&ab, "\x1b[?25l", 6);

    //'H' positions the cursor. It takes to arguments, specifying the line and column, say "/x1b[1;1H"
    //Line and column number start at 1
    abAppend(&ab, "\x1b[H", 3);
    
    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    //"?25h" shows the cursor after refreshing
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editor_set_status_message(const char *fmt, ...)
{
    //va_list is a pointer that points to the variable parameters
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

char *editor_prompt(char *prompt, void (*callback)(char *, int))
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0]= '\0';

    while(1) {
        editor_set_status_message(prompt, buf);
        editor_refresh_screen();
        int c = editor_read_key();
        
        if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if(buflen !=0 ) buf[--buflen] = '\0';
        } else if(c == '\x1b') {
            editor_set_status_message("");
            if(callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if(c == '\r') {
            if(buflen != 0) {
                editor_set_status_message("");
                if(callback) callback(buf, c);
                return buf;
            }
        } else if(!iscntrl(c) && c < 128) {
            if(buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if(callback) callback(buf, c);
    }
}

void editor_move_cursor(int key)
{
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch(key) {
        case ARROW_LEFT:
            if(E.cx != 0)
                E.cx--;
            else if(E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if(row && E.cx < row->size)
                E.cx++;
            else if(row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if(E.cy != 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if(E.cy < E.numrows) { //E.cy == E.numrows - 1 -> reach the end of the file
                E.cy++;
            }
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if(E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void editor_process_keypress() 
{
    static int quit_times = KILO_QUIT_TIMES;
    int c = editor_read_key();

    switch(c) {
        case '\r':
            editor_insert_new_line();
            break;

        case CTRL_KEY('q'):
            if(E.dirty && quit_times > 0) {
                editor_set_status_message("WARNING!!! File has unsaved changes. Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
        break;

        case CTRL_KEY('s'):
            editor_save();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            if(E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;

        case CTRL_KEY('f'):
            editor_find();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if(c == DEL_KEY) editor_move_cursor(ARROW_RIGHT);
            editor_del_char();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            { //the braces are for declaration of variables
                if(c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if(E.cy > E.numrows) E.cy = E.numrows;
                }
                int times = E.screenrows;
                while(times--)
                    editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editor_move_cursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;
        
        default:
            editor_insert_char(c);
            break;
    }
}

/*** init ***/
void init_editor() 
{
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;  //scroll to the top of the file by default
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename =  NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if(get_window_size(&E.screenrows, &E.screencols) == -1) die("get_window_size");
    E.screenrows -= 2;
}

int main(int argc, char *argv[]) 
{
    enable_raw_mode();
    init_editor();
    if(argc >= 2)
        editor_open(argv[1]);

    editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while(1) {
        editor_refresh_screen();
        editor_process_keypress();
    }
    
    return 0;
}