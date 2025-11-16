#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <asm-generic/errno-base.h>
#include <asm-generic/ioctls.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3
#define CTRL_KEY(k) ((k) & 0x1f)
#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

// TODO: Colorful serch results
struct editorSyntax {
    char *filetype;
    char **filematch;
    char **keywords;
    char *single_line_comment;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_open_comment;
} erow;

typedef struct editorConfig {
    int cursorX, cursorY;
    int screenRows;
    int rx;
    int screenColumns;
    int rowoff;
    int coloff;
    int dirty;
    struct termios orig_termios;
    int numrows;
    erow *row;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
} editorConfig;

typedef struct abuf {
    char *b;
    int len;
} abuf;

enum editorKey {
    BACKSPACE = 127,
    DEL_KEY,
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

enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define ABUF_INIT {NULL, 0}

editorConfig E;

char *extensions[] = {".c", ".h", ".cpp", NULL};
char *keyword[] = {"switch",    "if",      "while",   "for",    "break",
                   "continue",  "return",  "else",    "struct", "union",
                   "typedef",   "static",  "enum",    "class",  "case",
                   "int|",      "long|",   "double|", "float|", "char|",
                   "unsigned|", "signed|", "void|",   NULL};
struct editorSyntax HLDB[] = {
    {"c", extensions, keyword, "//", "/*", "*/",
     HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
};
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

// FUNCTIONS

void die(const char *function_name);
void enableRawMode();
void disableRawMode();
int editorKeyRead();
void editorDrawRows(abuf *buffer);
void editorDrawStatusBar(abuf *buffer);
void editorProcessKeyPress();
void editorRefreshScreen();
int getWindowSize(int *rows, int *columns);
void initEditor();
int getCursorPosition(int *rows, int *columns);
void abAppend(abuf *buffer, const char *string, int len);
void abFree(abuf *buffer);
void editorMoveCursor(int key);
void editorOpen(char *filename);
void editorInsertRow(int at, char *str, size_t len);
void editorScroll();
void editorUpdateRow(erow *row);
int editorRowCxToRx(erow *row, int cursorX);
int editorRowRxToCx(erow *row, int rx);
void editorSetStatusMessage(const char *fmt, ...);
void editorDrawStatusMessage(abuf *buffer);
void editorRowInsertChar(erow *row, int at, int c);
void editorInsertChar(int c);
char *editorRowsToString(int *buflen);
void editorSave();
void editorDelChar();
void editorRowDelChar(erow *row, int at);
void editorFreeRow(erow *row);
void editorDelRow(int at);
void editorRowAppendString(erow *row, char *s, size_t len);
void editorInsertNewLine();
char *editorPrompt(char *prompt, void (*callback)(char *, int));
void editorFind();
void editorFindCallback(char *query, int key);
void editorUpdateSyntax(erow *row);
int editorSyntaxToColor(int hl);
int is_separator(int c);
void editorSelectSyntaxHighlight();

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage(
        "HELP: Ctrl-Q = quit | Ctrl-S = save | Ctrl-f = find");
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
    static int quit_times = KILO_QUIT_TIMES;
    int c = editorKeyRead();

    switch (c) {
    case '\r':
        editorInsertNewLine();
        break;
    case DEL_KEY:
    case BACKSPACE:
    case CTRL('h'):
        if (c == DEL_KEY)
            editorMoveCursor(ARROW_RIGHT);
        editorDelChar();
        break;
    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0) {
            editorSetStatusMessage(
                "WARNING!!! File is unsaved use Ctrl-Q: %d times to quit",
                quit_times);
            quit_times--;
            return;
        }
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;
    case CTRL('l'):
    case '\x1b':
        break;
    case CTRL('s'):
        editorSave();
        break;
    case END:
        if (E.cursorY < E.numrows)
            E.cursorX = E.row[E.cursorY].size;
        break;
    case HOME:
        E.cursorX = 0;
        break;
    case PAGE_UP:
    case PAGE_DOWN: {
        if (c == PAGE_UP) {
            E.cursorY = E.rowoff;
        } else if (c == PAGE_DOWN) {
            E.cursorY = E.rowoff + E.screenColumns - 1;
            if (E.cursorY < E.numrows)
                E.cursorY = E.numrows;
        }
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
    case CTRL('f'):
        editorFind();
        break;
    default:
        editorInsertChar(c);
        break;
    }
    quit_times = KILO_QUIT_TIMES;
}

void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);
    editorSelectSyntaxHighlight();
    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        if (linelen != -1) {
            while (linelen > 0 &&
                   (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
                linelen--;
            editorInsertRow(E.numrows, line, linelen);
        }
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorRefreshScreen() {
    editorScroll();
    abuf buffer = ABUF_INIT;
    char buf[32];

    abAppend(&buffer, "\x1b[?25l", 6);
    abAppend(&buffer, "\x1b[H", 3);
    editorDrawRows(&buffer);
    editorDrawStatusBar(&buffer);
    editorDrawStatusMessage(&buffer);
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursorY - E.rowoff) + 1,
             (E.rx - E.coloff) + 1);
    abAppend(&buffer, buf, strlen(buf));
    abAppend(&buffer, "\x1b[?25h", 6);
    write(STDOUT_FILENO, buffer.b, buffer.len);
    abFree(&buffer);
}
void editorDrawRows(abuf *buffer) {
    for (int i = 0; i < E.screenRows; i++) {
        int filerow = i + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && i == E.screenRows / 3) {
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
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0)
                len = 0;
            if (len > E.screenColumns)
                len = E.screenColumns;
            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;
            for (int i = 0; i < len; i++) {
                if (iscntrl(c[i])) {
                    char sym = (c[i] <= 26) ? '@' + c[i] : '?';
                    abAppend(buffer, "\x1b[7m", 4);
                    abAppend(buffer, &sym, 1);
                    abAppend(buffer, "\x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm",
                                            current_color);
                        abAppend(buffer, buf, clen);
                    }
                } else if (hl[i] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(buffer, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(buffer, &c[i], 1);
                } else {
                    int color = editorSyntaxToColor(hl[i]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[32];
                        int clen =
                            snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(buffer, buf, clen);
                    }
                    abAppend(buffer, &c[i], 1);
                }
            }
            abAppend(buffer, "\x1b[39m", 5);
        }
        abAppend(buffer, "\x1b[K", 3);
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
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;

    if (getWindowSize(&E.screenRows, &E.screenColumns) == -1)
        die("getWindowSize");
    E.screenRows -= 2;
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
    erow *row = (E.cursorY >= E.numrows) ? NULL : &E.row[E.cursorY];
    switch (key) {
    case ARROW_UP:
        if (E.cursorY != 0)
            E.cursorY--;
        break;
    case ARROW_DOWN:
        if (E.cursorY < E.numrows)
            E.cursorY++;
        break;
    case ARROW_LEFT:
        if (E.cursorX != 0)
            E.cursorX--;
        else if (E.cursorY > 0) {
            E.cursorY--;
            E.cursorX = E.row[E.cursorY].size;
        }
        break;
    case ARROW_RIGHT:
        if (row && E.cursorX < row->size)
            E.cursorX++;
        else if (E.cursorY > 0) {
            E.cursorY--;
            E.cursorX = E.row[E.cursorY].size;
        }
        break;
    }
    row = (E.cursorY >= E.numrows) ? NULL : &E.row[E.cursorY];
    int rowlen = row ? row->size : 0;
    if (E.cursorX > rowlen)
        E.cursorX = rowlen;
}
void editorInsertRow(int at, char *str, size_t len) {
    if (at < 0 || E.numrows < at)
        return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++)
        E.row[j].idx++;

    E.row[at].idx = at;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, str, len);
    E.row[at].chars[len] = '\0';
    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]);
    E.numrows++;
    E.dirty++;
}

void editorScroll() {
    E.rx = 0;
    if (E.cursorY < E.numrows)
        E.rx = editorRowCxToRx(&E.row[E.cursorY], E.cursorX);
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.cursorY < E.rowoff) {
        E.rowoff = E.cursorY;
    }
    if (E.rx >= E.coloff + E.screenColumns) {
        E.coloff = E.rx - E.screenColumns + 1;
    }
    if (E.cursorY >= E.rowoff + E.screenRows) {
        E.rowoff = E.cursorY - E.screenRows + 1;
    }
}
void editorUpdateRow(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t')
            tabs++;
    }
    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);
    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0)
                row->render[idx++] = ' ';
        } else
            row->render[idx++] = row->chars[j];
    }
    row->render[idx] = '\0';
    row->rsize = idx;
    editorUpdateSyntax(row);
}

int editorRowCxToRx(erow *row, int cursorX) {
    int rx = 0;
    for (int j = 0; j < cursorX; j++) {
        if (row->chars[j] == '\t')
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

void editorDrawStatusBar(abuf *buffer) {
    abAppend(buffer, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
                        E.syntax ? E.syntax->filetype : "no ft", E.cursorY + 1,
                        E.numrows);
    int len = snprintf(status, sizeof(status), "%.20s - %d Lines %s",
                       E.filename ? E.filename : "[No Name]", E.numrows,
                       E.dirty ? "(modified)" : " ");
    if (len > E.screenColumns)
        len = E.screenColumns;
    abAppend(buffer, status, len);
    while (len < E.screenColumns) {
        if (E.screenColumns - len == rlen) {
            abAppend(buffer, rstatus, rlen);
            break;
        } else {
            abAppend(buffer, " ", 1);
            len++;
        }
    }
    abAppend(buffer, "\x1b[m", 3);
    abAppend(buffer, "\r\n", 2);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

void editorDrawStatusMessage(abuf *buffer) {
    abAppend(buffer, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screenColumns)
        msglen = E.screenColumns;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(buffer, E.statusmsg, msglen);
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size)
        at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorInsertChar(int c) {
    if (E.cursorY == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cursorY], E.cursorX, c);
    E.cursorX++;
}

char *editorRowsToString(int *buflen) {
    int totlen = 0;
    for (int i = 0; i < E.numrows; i++) {
        totlen += E.row[i].size + 1;
    }
    *buflen = totlen;
    char *buf = malloc(totlen);
    char *p = buf;
    for (int i = 0; i < E.numrows; i++) {
        memcpy(p, E.row[i].chars, E.row[i].size);
        p += E.row[i].size;
        *p = '\n';
        p++;
    }
    return buf;
}
void editorSave() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
        editorSelectSyntaxHighlight();
    }
    int len;
    char *buf = editorRowsToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written on disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Cant save! I/O error", strerror(errno));
}

void editorDelChar() {
    if (E.cursorY == E.numrows)
        return;
    if (E.cursorX == 0 && E.cursorY == 0)
        return;
    erow *row = &E.row[E.cursorY];
    if (E.cursorX > 0) {
        editorRowDelChar(row, E.cursorX - 1);
        E.cursorX--;
    } else {
        E.cursorX = E.row[E.cursorY - 1].size;
        editorRowAppendString(&E.row[E.cursorY - 1], row->chars, row->size);
        editorDelRow(E.cursorY);
        E.cursorY--;
    }
}
void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at > E.row->size)
        return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->chars);
    free(row->render);
    free(row->hl);
}
void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows)
        return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    for (int i = at; i < E.numrows - 1; i++) {
        E.row[i].idx--;
    }
    E.numrows--;
    E.dirty++;
}
void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}
void editorInsertNewLine() {
    if (E.cursorX == 0) {
        editorInsertRow(E.cursorY, "", 0);
    } else {
        erow *row = &E.row[E.cursorY];
        editorInsertRow(E.cursorY + 1, &row->chars[E.cursorX],
                        row->size - E.cursorX);
        row = &E.row[E.cursorY];
        row->size = E.cursorX;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cursorY++;
    E.cursorX = 0;
}

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';
    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();
        int c = editorKeyRead();
        if (c == DEL_KEY || c == BACKSPACE || c == CTRL('h')) {
            if (buflen != 0)
                buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback)
                callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback)
                    callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback)
            callback(buf, c);
    }
}

void editorFindCallback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;
    static int saved_hl_line;
    static char *saved_hl = NULL;
    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }
    if (key == '\x1b' || key == '\r') {
        last_match = -1;
        direction = -1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        direction = 1;
        last_match = -1;
    }
    if (last_match == -1)
        direction = 1;
    int current = last_match;
    for (int i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1)
            current = E.numrows - 1;
        else if (current == E.numrows)
            current = 0;
        erow *row = &E.row[current];
        char *match = strstr(row->chars, query);
        if (match) {
            last_match = current;
            E.cursorY = current;
            E.cursorX = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;
            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind() {
    int saved_cursorX = E.cursorX;
    int saved_cursorY = E.cursorY;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;
    char *query =
        editorPrompt("Find: %s (Use ESC/Arrows/Enter)", editorFindCallback);
    if (query)
        free(query);
    else {
        E.cursorX = saved_cursorX;
        E.cursorY = saved_cursorY;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cursorX;
    for (cursorX = 0; cursorX < row->size; cursorX++) {
        if (row->chars[cursorX] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        cur_rx++;
        if (cur_rx > rx)
            return cursorX;
    }
    return cursorX;
}

void editorUpdateSyntax(erow *row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);
    if (E.syntax == NULL)
        return;
    char **keywords = E.syntax->keywords;
    char *scs = E.syntax->single_line_comment;
    char *mcs = E.syntax->multiline_comment_end;
    char *mce = E.syntax->multiline_comment_start;
    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mcs ? strlen(mce) : 0;
    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);
    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;
        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->size - i);
                break;
            }
        }
        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_COMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }
        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->size) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string)
                    in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }
        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2)
                    klen--;
                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen])) {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }
        prev_sep = is_separator(c);
        i++;
    }
    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.numrows)
        editorUpdateSyntax(&E.row[row->idx + 1]);
}

int editorSyntaxToColor(int hl) {
    switch (hl) {
    case HL_KEYWORD1:
        return 33;
    case HL_KEYWORD2:
        return 32;
    case HL_MLCOMMENT:
    case HL_COMMENT:
        return 36;
    case HL_NUMBER:
        return 34;
    case HL_STRING:
        return 35;
    case HL_MATCH:
        return 31;
    default:
        return 37;
    };
}

int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) == NULL;
}

void editorSelectSyntaxHighlight() {
    E.syntax = NULL;
    if (E.filename == NULL)
        return;
    char *ext = strchr(E.filename, '.');
    for (unsigned int i = 0; i < HLDB_ENTRIES; i++) {
        struct editorSyntax *s = &HLDB[i];
        unsigned int j = 0;
        while (s->filematch[j]) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[j])) ||
                (!is_ext && strstr(E.filename, s->filematch[j]))) {
                E.syntax = s;
                for (int filerow = 0; filerow < E.numrows; filerow++) {
                    editorUpdateSyntax(&E.row[filerow]);
                }
                return;
            }
        }
    }
}
