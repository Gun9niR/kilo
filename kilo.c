/*** includes ***/
#include<ctype.h>
#include<errno.h>
#include<stdio.h>
#include<stdlib.h>
#include<termios.h>
#include<unistd.h>

/*** data ***/

struct termios orig_terminos;

/*** terminal ***/

void die(const char *s) {
    /*
    perror() looks at the global errno variable and prints a descriptive error message for it. 
    t also prints the string given to it before it prints the error message, 
    which is meant to provide context about what part of your code caused the error.
    */
    perror(s);
    exit(1);
}

void disable_raw_mode()
{
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_terminos) == -1)
        die("tcsetattr");
}

void enable_raw_mode() 
{
    if(tcgetattr(STDIN_FILENO, &orig_terminos) == -1) die("tcgetattr");
    atexit(disable_raw_mode); //register the function to be called when program exits

    struct termios raw = orig_terminos;
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

/*** init ***/

int main() {
    enable_raw_mode();

    while(1) {
        char c = '\0';
        if(read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");
        if(iscntrl(c))
            printf("%d\r\n",c);
        else 
            printf("%d ('%c')\r\n", c, c);
        if(c == 'q') break;
    }

    return 0;
}