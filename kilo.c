/*** includes ***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.0"

// Masks first 5 bits of character to convert char to C-char
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
	ARROW_LEFT = 1000, 
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN
};

/*** data ***/
struct editorConfig {
	int cx, cy;
	struct termios orig_termios;
	int screenrows;
	int screencols;
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
			switch (seq[1]) {
				case 'A': return ARROW_UP;
				case 'B': return ARROW_DOWN;
				case 'C': return ARROW_RIGHT;
				case 'D': return ARROW_LEFT;
			}
		}
	}
	return c;
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

void editorDrawRows(struct appendbuf *ab) {
	int y;
	// draw 24 tildes
	for (y = 0; y < config.screenrows; y++) {
		if (y == config.screenrows / 3) {
			char welcome[80];
			int welcomelen = snprintf(welcome, sizeof(welcome),
				"Kilo editor -- version %s", KILO_VERSION);
			if (welcomelen > config.screencols) welcomelen = config.screencols;
			int padding = (config.screencols - welcomelen) / 2;
			if (padding) {
				appendToBuffer(ab, "~", 1);
				padding--;
			}
			while (padding --) appendToBuffer(ab, " ", 1);
			appendToBuffer(ab, welcome, welcomelen);
		} else {
			appendToBuffer(ab, "~", 1);
		}

		appendToBuffer(ab, "\x1b[K", 3);
		if (y < config.screenrows -1) {
			appendToBuffer(ab, "\r\n", 2);
		}	
	}
}

void editorRefreshScreen() {
	struct appendbuf ab = ABUF_INIT;

	// hide cursor and move cursor top left
	appendToBuffer(&ab, "\x1b[?25l", 6);
	appendToBuffer(&ab, "\x1b[H", 3);

	// draw
	editorDrawRows(&ab);
	
	// move cursor to current position
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", config.cy + 1, config.cx + 1);
	appendToBuffer(&ab, buf, strlen(buf));

	// show cursor
	appendToBuffer(&ab, "\x1b[?25h", 6);

	// write and erase buffer
	write(STDOUT_FILENO, ab.b, ab.len);
	freeAppendBuffer(&ab);
}

/*** input ***/

void editorMoveCursor(int key) {
	switch (key) {
		case ARROW_LEFT: 
			config.cx--;
			break;
		case ARROW_RIGHT:
			config.cx++;
			break;
		case ARROW_UP:
			config.cy--;
			break;
		case ARROW_DOWN:
			config.cy++;
			break;
	}
}

void editorProcessKeypress() {
	int c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			// Clear Screen
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
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
	config.cx = 0;
	config.cy = 0;

	if (getWindowSize(&config.screenrows, &config.screencols) == -1) 
		die("getWindowSize");
}
int main() {
	enableRawMode();
	initEditor();

	// runtime loop
	while(1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}
	return 0; 
}

