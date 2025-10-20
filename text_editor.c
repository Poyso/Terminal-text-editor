#include <asm-generic/errno-base.h>
#include <asm-generic/ioctls.h>
// #include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// STEP 58

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

typedef struct erow {
    int size;
    char *chars;
} erow;

typedef struct editorConfig {
    int cursorX, cursorY;
    int screenRows;
    int screenColumns;
    struct termios orig_termios;
    int numrows;
    erow row;
} editorConfig;

typedef struct abuf {
    char *b;
    int len;
} abuf;

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL,
    HOME,
    END,
    PAGE_UP,
    PAGE_DOWN,
};

#define ABUF_INIT {NULL, 0}

editorConfig E;

void die(const char *function_name);
void enableRawMode();
void disableRawMode();
int editorKeyRead();
void editorDrawRows(abuf *buffer);
void editorProcessKeyPress();
void editorRefreshScreen();
int getWindowSize(int *rows, int *columns);
void initEditor();
int getCursorPosition(int *rows, int *columns);
void abAppend(abuf *buffer, const char *string, int len);
void abFree(abuf *buffer);
void editorMoveCursor(int key);
void editorOpen();

int main() {
    enableRawMode();
    initEditor();
    editorOpen();

    while (1) {
        editorRefreshScreen();
        editorProcessKeyPress();
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr"); // Reads terminal attributes
    atexit(disableRawMode);
    struct termios raw = E.orig_termios;
    // ECHO: u cant see on the terminal the key pressed (like when u type
    // password for sudo)
    // ICANON: disabling this enables the byte-by-byte reading instead of the
    // line-by-line
    raw.c_iflag &= ~(BRKINT | INPCK | IXON | ICRNL | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag &= ~(CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr"); // sets the attributes
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
} // set default attributes of the terminal

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

int editorKeyRead() {
    char c;
    int nread;
    while ((nread = read(STDIN_FILENO, &c, 1) != 1)) {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9')
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
            if (seq[2] == '~')
                switch (seq[1]) {
                case '1':
                    return HOME;
                case '3':
                    return DEL;
                case '4':
                    return END;
                case '5':
                    return PAGE_UP;
                case '6':
                    return PAGE_DOWN;
                case '7':
                    return HOME;
                case '8':
                    return END;
                }
            else {
                switch (seq[1]) {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME;
                case 'F':
                    return END;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
            case 'H':
                return HOME;
            case 'F':
                return END;
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}
void editorProcessKeyPress() {
    int c = editorKeyRead();

    switch (c) {
    case CTRL_KEY('q'):
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;
    case END:
        E.cursorX = E.screenColumns - 1;
        break;
    case HOME:
        E.cursorX = 0;
        break;
    case PAGE_UP:
    case PAGE_DOWN: {
        int times = E.screenRows;
        while (times--)
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);

    } break;
    case ARROW_DOWN:
    case ARROW_RIGHT:
    case ARROW_UP:
    case ARROW_LEFT:
        editorMoveCursor(c);
        break;
    }
}

void editorOpen(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");
    char *line = NULL;
    ssize_t linecap = 0;
    ssize_t linelen;
    E.row.size = linelen;
    E.row.chars = malloc(linelen + 1);
    memcpy(E.row.chars, line, linelen);
    E.row.chars[linelen] = '\0';
    E.numrows = 1;
}

void editorRefreshScreen() {
    abuf buffer = ABUF_INIT;
    char buf[32];

    abAppend(&buffer, "\x1b[?25l", 6);
    abAppend(&buffer, "\x1b[H", 3);
    editorDrawRows(&buffer);
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cursorY + 1, E.cursorX + 1);
    abAppend(&buffer, buf, strlen(buf));
    abAppend(&buffer, "\x1b[?25h", 6);
    write(STDOUT_FILENO, buffer.b, buffer.len);
    abFree(&buffer);
}
void editorDrawRows(abuf *buffer) {
    for (int i = 0; i < E.screenRows; i++) {
        if (i >= E.numrows) {
            if (i == E.screenRows / 3) {
                char welcome[80];
                int welcomelen =
                    snprintf(welcome, sizeof(welcome),
                             "Kilo Editor -- version %s", KILO_VERSION);
                if (welcomelen > E.screenColumns)
                    welcomelen = E.screenColumns;
                int padding = (E.screenColumns - welcomelen) / 2;
                if (padding) {
                    abAppend(buffer, "~", 1);
                    padding--;
                }
                while (padding--)
                    abAppend(buffer, " ", 1);
                abAppend(buffer, welcome, welcomelen);
            } else {
                abAppend(buffer, "~", 1);
            }
        } else {
            int len = E.row.size;
            if (len > E.screenColumns)
                len = E.screenColumns;
            abAppend(buffer, E.row.chars, len);
        }
        abAppend(buffer, "\x1b[K", 3);
        if (i < E.screenRows - 1)
            abAppend(buffer, "\r\n", 2);
    }
}

int getWindowSize(int *rows, int *column) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1 || w.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(rows, column);

    } else {
        *rows = w.ws_row;
        *column = w.ws_col;
        return 0;
    }
}

void initEditor() {
    E.cursorX = 0;
    E.cursorY = 0;
    E.numrows = 0;

    if (getWindowSize(&E.screenRows, &E.screenColumns) == -1)
        die("getWindowSize");
}

int getCursorPosition(int *rows, int *columns) {
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;
    printf("\r\n");
    char c;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &c, 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, columns))
        return -1;
    return 0;
}

void abAppend(abuf *buffer, const char *string, int len) {
    char *new = realloc(buffer->b, buffer->len + len);
    if (new == NULL)
        return;
    memcpy(&new[buffer->len], string, len);
    buffer->b = new;
    buffer->len += len;
}
void abFree(abuf *buffer) { free(buffer->b); }

void editorMoveCursor(int key) {
    switch (key) {
    case ARROW_UP:
        if (E.cursorY != 0)
            E.cursorY--;
        break;
    case ARROW_DOWN:
        if (E.cursorY < E.screenRows - 1)
            E.cursorY++;
        break;
    case ARROW_LEFT:
        if (E.cursorX != 0)
            E.cursorX--;
        break;
    case ARROW_RIGHT:
        if (E.cursorX < E.screenColumns - 1)
            E.cursorX++;
        break;
    }
}
