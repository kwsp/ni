/*** includes ***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termio.h>
#include <unistd.h>

/***defines ***/

#define NI_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKeys {
    ARROW_LEFT = 'h',
    ARROW_RIGHT = 'l',
    ARROW_UP = 'k',
    ARROW_DOWN = 'j',
};

/*** data ***/

struct editorConfig {
    int cx, cy;
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

    // Process arrow keys by immediately reading the next
    // two bytes after an ESC press
    //
    // We'll handle longer escape sequences in the future
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
            }
        }

        return '\x1b';
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

/*** dynamic string ***/
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

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
void editorDrawRows(struct abuf *ab) {
    int y;
    for (y=0; y < E.screenrows; y++) {

        if (y == E.screenrows / 3) {
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome),
                    "Ni editor -- version %s", NI_VERSION);
            if (welcomelen > E.screencols) welcomelen = E.screencols;

            int padding = (E.screencols - welcomelen) / 2;
            if (padding) {
                abAppend(ab, "~", 1);
                padding--;
            }
            while (padding--) abAppend(ab, " ", 1);

            abAppend(ab, welcome, welcomelen);

        } else {
            abAppend(ab, "~", 1);
        }

        // Clear line
        abAppend(ab, "\x1b[K", 3);
        if (y < E.screenrows -1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    // Hide cursor
    abAppend(&ab, "\x1b[?25l", 6);
    // Reset cursor
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    // Set cursor position
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    // Show cursor
    abAppend(&ab, "\x1b[?25h", 6);

    // Write buffer
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

void editorMoveCursor(char key) {
    switch (key) {
        case ARROW_UP:
            E.cy--;
            break;
        case ARROW_DOWN:
            E.cy++;
            break;
        case ARROW_LEFT:
            E.cx--;
            break;
        case ARROW_RIGHT:
            E.cx++;
            break;
    }
}

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

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;

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
