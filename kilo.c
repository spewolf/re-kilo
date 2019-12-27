/*** includes ***/

// Provides access to lower level functions
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.0"
#define TAB_STOP 8
// Masks first 5 bits of character to convert char to C-char
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
	ARROW_LEFT = 1000, 
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	PAGE_UP,
	PAGE_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY
};

/*** data ***/

typedef struct erow {
	int size;
	int rsize;
	char *chars;
	char *render;
} erow;

struct editorConfig {
	int cx, cy;
	int rx;
	int rowoff;
	int coloff;
	struct termios orig_termios;
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
};

struct editorConfig config;

/*** terminal ***/

// perror will print given string and then detailed discription of error based on global errno
void die (const char *err) {
	// Clear Screen
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	perror(err);
	exit(1);
}

// returns terminal to orriginal attributes
void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &config.orig_termios) == -1)
		die("tcsetattr");
}

// disables default behaviors of terminal such as cannonical mode
void enableRawMode() {
	if (tcgetattr(STDIN_FILENO, &config.orig_termios) == -1) die("tcgetattr");
	// atexit calls function upon program exit
	atexit(disableRawMode);

	// flags are set via bitwise operations
	struct termios raw = config.orig_termios;
	raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

	// Minimum bytes to return from read()
	raw.c_cc[VMIN] = 0;
	// Time to wait till returning from read() in 1/10ths of a second
	raw.c_cc[VTIME] = 1;

	// attributes are set via terminal control
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// read character from terminal input
int editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
					switch (seq[1]) {
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
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}
	
		return '\x1b';
	} else {
		return c;
	}
}

int getCursorPosition(int *rows, int *cols) {
	char buf[32];
	unsigned int i = 0;

	// ask for n (device status report) 6 (cursor position)
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	//read result into buffer
	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if(buf[i] == 'R') break;
		i++;
	}
	// terminate string
	buf[i] = '\0';

	// parse pointer location
	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	// read window size
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		// given a failure, move pointer to bottom right and read position
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows,cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t')
			rx += (TAB_STOP - 1) - (rx % TAB_STOP);
		rx++;
	}
	return rx;
}

void editorUpdateRow(erow *row) {
	int tabs = 0;
	int j;
	for(j = 0; j < row->size; j++)
		if (row->chars[j] == '\t') tabs++;

	free(row->render);
	row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

	int idx = 0;
	for(j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
	config.row = realloc(config.row, sizeof(erow) * (config.numrows + 1));

	int at = config.numrows;
	config.row[at].size = len;
	config.row[at].chars = malloc(len + 1);
	memcpy(config.row[at].chars, s, len);
	config.row[at].chars[len] = '\0';

	config.row[at].rsize = 0;
	config.row[at].render = NULL;
	editorUpdateRow(&config.row[at]);

	config.numrows++;
}

/*** file I/O ***/

void editorOpen(char* filename) {
	FILE *fp = fopen(filename, "r");
	if (!fp) die("fopen");

	char *line = NULL; 
	size_t linecap = 0;
	ssize_t linelen;
	linelen = getline(&line, &linecap, fp);
	// add lines from file to rows
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
							line[linelen - 1] == '\r'))
			linelen--;
		editorAppendRow(line, linelen);
	}
	free(line);
	fclose(fp);
}

/*** append ***/

struct appendbuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

// appends a string to the buffer
void appendToBuffer(struct appendbuf *ab, const char *s, int len) {
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;
	// copy string to memory after ab
	memcpy(&new[ab->len], s, len);
	// assign new pointer to struct in case of location change
	ab->b = new;
	// increase length of buffer
	ab->len += len;
}


// deallocate buffer
void freeAppendBuffer(struct appendbuf *ab) {
	free(ab->b);
}

/*** output ***/

void editorScroll() {
	config.rx = 0;
	if (config.cy < config.numrows) {
		config.rx = editorRowCxToRx(&config.row[config.cy], config.cx);
	}

	if (config.cy < config.rowoff) {
		config.rowoff = config.cy;
	}
	if (config.cy >= config.rowoff + config.screenrows) {
		config.rowoff = config.cy - config.screenrows + 1;
	}
	if (config.rx < config.coloff) {
		config.coloff = config.rx;
	}
	if (config.rx >= config.coloff + config.screencols) {
		config.coloff = config.rx - config.screencols + 1;
	}
}

void editorDrawRows(struct appendbuf *ab) {
	int y;
	// draw 24 tildes
	for (y = 0; y < config.screenrows; y++) {
		int filerow = y + config.rowoff;
		// draw version screen and tildes below text
		if(filerow >= config.numrows) {
			if (config.numrows == 0 && y == config.screenrows / 3) {
				char welcome[80];
				int welcomelen = snprintf(welcome, sizeof(welcome),
					"Kilo editor -- version %s", KILO_VERSION);
				if (welcomelen > config.screencols) welcomelen = config.screencols;
				int padding = (config.screencols - welcomelen) / 2;
				if (padding) {
					appendToBuffer(ab, "~", 1);
					padding--;
				}
				while (padding--) appendToBuffer(ab, " ", 1);
				appendToBuffer(ab, welcome, welcomelen);
			} else {
				appendToBuffer(ab, "~", 1);
			}
		// else print rows of text
		} else {
			int len = config.row[filerow].rsize - config.coloff;
			if (len < 0) len = 0;
			if (len > config.screencols) len = config.screencols;
			appendToBuffer(ab, &config.row[filerow].render[config.coloff], len);
		}

		appendToBuffer(ab, "\x1b[K", 3);
		if (y < config.screenrows - 1) {
			appendToBuffer(ab, "\r\n", 2);
		}	
	}
}

void editorRefreshScreen() {
	editorScroll();

	struct appendbuf ab = ABUF_INIT;

	// hide cursor and move cursor top left
	appendToBuffer(&ab, "\x1b[?25l", 6);
	appendToBuffer(&ab, "\x1b[H", 3);

	// draw
	editorDrawRows(&ab);
	
	// move cursor to current position
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", 
	         (config.cy - config.rowoff) + 1,
			 (config.rx - config.coloff) + 1);
	appendToBuffer(&ab, buf, strlen(buf));

	// show cursor
	appendToBuffer(&ab, "\x1b[?25h", 6);

	// write and erase buffer
	write(STDOUT_FILENO, ab.b, ab.len);
	freeAppendBuffer(&ab);
}

/*** input ***/

void editorMoveCursor(int key) {
	erow *row = (config.cy >= config.numrows) ? NULL : &config.row[config.cy];

	switch (key) {
		case ARROW_LEFT: 
			if (config.cx != 0) config.cx--;
			else if (config.cy > 0) {
				config.cy--;
				config.cx = config.row[config.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && config.cx < row->size) config.cx++;
			else if (row && config.cx == row->size) {
				config.cy++;
				config.cx = 0;
			}
			break;
		case ARROW_UP:
			if (config.cy != 0) config.cy--;
			break;
		case ARROW_DOWN:
			if (config.cy < config.numrows) config.cy++;
			break;
	}

	row = (config.cy >= config.numrows) ? NULL : &config.row[config.cy];
	int rowlen = row ? row->size : 0;
	if (config.cx > rowlen) {
		config.cx = rowlen;
	}
}

void editorProcessKeypress() {
	int c = editorReadKey();

	switch (c) {
		// Exit key
		case CTRL_KEY('q'):
			// Clear Screen
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		// Move keys
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;

		// Navigate keys
		case HOME_KEY:
			config.cx = 0;
			break;
		case END_KEY:
			config.cx = config.screencols - 1;
			break;
		case PAGE_UP:
		case PAGE_DOWN:
			{
				int times = config.screenrows;
				while (times--) {
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
				}
			}
			break;
	}
}

/*** init ***/

void initEditor() {
	config.cx = 0;
	config.cy = 0;
	config.rx = 0;
	config.rowoff = 0;
	config.coloff = 0;
	config.numrows = 0;
	config.row = NULL;

	if (getWindowSize(&config.screenrows, &config.screencols) == -1) 
		die("getWindowSize");
}
int main(int argc, char *argv[]) {
	enableRawMode();
	initEditor();
	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	// runtime loop
	while(1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}
	return 0; 
}

