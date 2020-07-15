/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include<ctype.h>
#include<errno.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/ioctl.h>
#include<sys/types.h>
#include<termios.h>
#include<unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)  //a bit mask that sets bits 5 and 6 bits of the character to 0，which is exactly how CTRL works
enum editorKey {
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
    char *chars;
}erow;

struct editor_config {
    int cx, cy;   //cursor position, starting at 0
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    struct termios orig_terminos;
};

struct editor_config E;

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

void editor_append_row(char *s, size_t len)
{
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

/*** file i/o ***/

void editor_open(char *filename) 
{
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
        editor_append_row(line, linelen);
    }
    free(line);
    fclose(fp);
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

void editor_draw_rows(struct abuf *ab)
{
    int y;
    for(y = 0; y < E.screenrows; y++) {
        if(y >= E.numrows) {
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
            int len = E.row[y].size;
            if(len > E.screencols) len = E.screencols;
            abAppend(ab, E.row[y].chars, len);
        }
        //clear each line as we redraw them instead of clearing the entire page
        abAppend(ab, "\x1b[K", 3); 
        if(y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
         
    }
}

void editor_refresh_screen()
{
    struct abuf ab = ABUF_INIT; //use buffer so that we only need to do one write(), avoiding flickering

    //"?25l" hides the cursor when refreshing the screen to prevent flickering
    abAppend(&ab, "\x1b[?25l", 6);

    //'H' positions the cursor. It takes to arguments, specifying the line and column, say "/x1b[1;1H"
    //Line and column number start at 1
    abAppend(&ab, "\x1b[H", 3);
    
    editor_draw_rows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    //"?25h" shows the cursor after refreshing
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

void editor_move_cursor(int key)
{
    switch(key) {
        case ARROW_LEFT:
            if(E.cx != 0)
                E.cx--;
            break;
        case ARROW_RIGHT:
            if(E.cx != E.screencols - 1)
                E.cx++;
            break;
        case ARROW_UP:
            if(E.cy != 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if(E.cy != E.screenrows - 1)
            E.cy++;
            break;
    }
}

void editor_process_keypress() 
{
    int c = editor_read_key();

    switch(c) {
        case CTRL_KEY('q'):
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            E.cx = E.screencols - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            { //the braces are for declaration of variables
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
    }
}

/*** init ***/
void initEditor() 
{
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;
    E.row = NULL;

    if(get_window_size(&E.screenrows, &E.screencols) == -1) die("get_window_size");
}

int main(int argc, char *argv[]) 
{
    enable_raw_mode();
    initEditor();
    if(argc >= 2)
        editor_open(argv[1]);

    while(1) {
        editor_refresh_screen();
        editor_process_keypress();
    }
    
    return 0;
}