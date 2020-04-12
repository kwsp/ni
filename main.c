/*** includes ***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termio.h>
#include <unistd.h>

/***defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/

struct editorConfig {
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

void editorClearScreen();

/*
 * Errorhandling.
 * Check each of our library calls for failure and call die when they fail
 */
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

/*
 * Restore terminal attributes before quitting the program
 */
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

/*
 * Turn off echo mode in the terminal
 */
void enableRawMode() {
    // Read the current attributes into a struct
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");

    // Register disableRawMode to be called at exit
    atexit(disableRawMode);

    // Copy the original attributes
    struct termios raw = E.orig_termios;

    // Disable input flags:
    //     IXON: Ctrl-S and Ctrl-Q (flow control)
    //     ICRNL: Stop translating carriage returns into newlines
    //            Ctrl-M and Enter both read 13, '\r' instead of 10, '\n'
    //     BRKINT: Stop break condition from sending SIGINT
    // Other misc flags:
    //     INPCK, ISTRIP: Flags required to enable raw mode in old
    //          terminals that are probably already disabled
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | ICRNL | IXON);

    // Disable output flags:
    //     OPOST: Stop translating newlines into carriage returns followed
    //            by a newline ('\n' to '\r\n')
    raw.c_oflag &= ~(OPOST);

    // Set character size to 8 bits per byte. Probably already set
    raw.c_cflag |= (CS8);

    // Disable local flags:
    //     ECHO mode (key presses aren't echoed)
    //     Canonical mode (each key press is passed to the program instead
    //         of new lines)
    //     ISIG: Ctrl-C and Ctrl-Z
    //     IEXTEN: Ctrl-V
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

    // Set a timeout for read() so we can do something else while waiting
    // Set the timeout to 1/10 of a second
    raw.c_cc[VTIME] = 1;
    // Set maximum amount of time read() waits before returning
    raw.c_cc[VMIN] = 0;

    // Write the new terminal attributes
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

/*
 * Reads a key press and return the char
 */
char editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }

    // Null terminate the string
    buf[i] = '\0';

    // Make sure it begins with <ESC>[
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // If sys/ioctl fail to give us the screen size,
        // we do it the hard way :)
        // Send the cursor to the bottom right corner of the screen and query
        // its position.
        //
        // There isn't a command to send the cursor to the bottom right of the screen :(
        // However, the C (cursor forward) and B (cursor down) escape sequences
        // are documented to stop the cursor from going past the edge of the
        // screen
        //
        // We don't use the H escape sequence because it is not documented
        // what happens when you move the cursor off screen.
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** output ***/

/*
 * Write an escape sequence to the screen.
 * Escape sequence begins with the "\x1b"
 * byte which means ESCAPE or 27 in decimal,
 * followed by '['.
 * '2J' clears the entire screen
 */

void editorClearScreen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

/*
 * Handle drawing each row of the text buffer being edited
 */
void editorDrawRows() {
    int y;
    for (y=0; y < E.screenrows; y++) {
        write(STDOUT_FILENO, "~", 1);

        if (y < E.screenrows -1) {
            write(STDOUT_FILENO, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    editorClearScreen();

    editorDrawRows();

    // Reset cursor position to 0,0
    write(STDOUT_FILENO, "\x1b[H", 3);
}

/*** input ***/

/*
 * Waits for a keypress and handles it
 */
void editorProcessKeypress() {
    char c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            editorClearScreen();
            exit(0);
            break;
    }
}

/*** init ***/

void initEditor() {
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
    enableRawMode();
    initEditor();

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    };

    return 0;
}
