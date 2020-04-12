#include <stdlib.h>
#include <termio.h>
#include <unistd.h>

struct termios orig_termios;

/*
 * Turn echo mode back on before quitting the program
 */
void disableRawMode() {
    struct termios raw;

    // Read the current attributes into a struct
    tcgetattr(STDIN_FILENO, &raw);

    // Set the ECHO flag off
    raw.c_lflag &= ~(ECHO);

    // Write the new terminal attributes
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/*
 * Turn off echo mode in the terminal
 */
void enableRawMode() {
    // Read the current attributes into a struct
    tcgetattr(STDIN_FILENO, &orig_termios);

    // Register disableRawMode to be called at exit
    atexit(disableRawMode);

    // Copy the original attributes
    struct termios raw = orig_termios;
    // Set the ECHO flag off
    raw.c_lflag &= ~(ECHO);

    // Write the new terminal attributes
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}


int main() {
    enableRawMode();

    char c;
    while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q') {

    };
    return 0;
}
