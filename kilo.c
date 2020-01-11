/*** includes ***/

// Provides access to lower level functions
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.0"
#define TAB_STOP 8
#define KILO_QUIT_TIMES 3
// Masks first 5 bits of character to convert char to C-char
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
	BACKSPACE = 127,
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

enum editorHighlight {
	HL_NORMAL = 0,
	HL_STRING,
	HL_NUMBER,
	HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** data ***/

struct editorSyntax {
	char *filetype;
	char **filematch;
	int flags;
};

typedef struct erow {
	int size;
	int rsize;
	char *chars;
	char *render;
	unsigned char *hl;
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
	char statusmsg[80];
	time_t statusmsg_time;
	erow *row;
	char *filename;
	int dirty;
	struct editorSyntax *syntax;
};

struct editorConfig config;

/*** filetypes ***/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };

struct editorSyntax HLDB[] = {
	{
		"c",
		C_HL_extensions,
		HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
	},
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

void editorSetMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

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

/*** syntax highlighting ***/

int is_seperator(int c) {
	return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
	row->hl = realloc(row->hl, row->rsize);
	memset(row->hl, HL_NORMAL, row->rsize);

	if (config.syntax == NULL) return;

	int prev_sep = 1;
	int in_string = 0;

	int i = 0;
	while (i < row->rsize) {
		char c = row->render[i];
		unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

		if (config.syntax->flags & HL_HIGHLIGHT_STRINGS) {
			if (in_string) {
				row->hl[i] = HL_STRING;
				if (c == '\\' && i + 1 < row->rsize) {
					row->hl[i + 1] = HL_STRING;
					i += 2;
					continue;
				}
				if (c == in_string) in_string = 0;
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
		
		if (config.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
			if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
							   (c == '.' && prev_hl == HL_NUMBER)) {
				row->hl[i] = HL_NUMBER;
				i++;
				prev_sep = 0;
				continue;
			}
		}

		prev_sep = is_seperator(c);
		i++;
	}
}

int editorSyntaxToColor(int hl) {
	switch (hl) {
		// ANSI majenta
		case HL_STRING: return 35;
		// ANSI foreground red
		case HL_NUMBER: return 31;
		// ANSI forground blue
		case HL_MATCH: return 34;
		// ANSI foreground white
		default: return 37;
	}
}

void editorSelectSyntaxHighlight() {
	config.syntax = NULL;
	if (config.filename == NULL) return;

	char *ext = strrchr(config.filename, '.');

	for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
		struct editorSyntax *s = &HLDB[j];
		unsigned int i = 0;
		while (s->filematch[i]) {
			int is_ext = (s->filematch[i][0] == '.');
			if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
			    (!is_ext && strstr(config.filename, s->filematch[i]))) {
				config.syntax = s;

				int filerow;
				for (filerow = 0; filerow < config.numrows; filerow++) {
					editorUpdateSyntax(&config.row[filerow]);
				}

				return;
			}
			i++;
		}
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

int editorRowRxToCx(erow *row, int rx) {
	int cur_rx = 0;
	int cx;
	for (cx = 0; cx < row->size; cx++) {
		if (row->chars[cx] == '\t')
			cur_rx += (TAB_STOP - 1) - (cur_rx % TAB_STOP);
		cur_rx++;

		if (cur_rx > rx) return cx;
	}
	return cx;
}

void editorUpdateRow(erow *row) {
	int tabs = 0;
	int j;
	for(j = 0; j < row->size; j++)
		if (row->chars[j] == '\t') tabs++;

	// Update render row
	free(row->render);
	row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

	// Convert tabs to spaces for rendering
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

	editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
	if (at < 0 || at > config.numrows) return;

	// Allocate erow and char memory
	config.row = realloc(config.row, sizeof(erow) * (config.numrows + 1));
	memmove(&config.row[at + 1], &config.row[at], sizeof(erow) * (config.numrows - at));
	config.row[at].size = len;
	config.row[at].chars = malloc(len + 1);
	// Insert data
	memcpy(config.row[at].chars, s, len);
	config.row[at].chars[len] = '\0';
	// Initialize render memory
	config.row[at].rsize = 0;
	config.row[at].render = NULL;
	config.row[at].hl = NULL;
	// Update editor
	editorUpdateRow(&config.row[at]);
	config.numrows++;
	config.dirty++;
}

void editorFreeRow(erow *row) {
	free(row->render);
	free(row->chars);
	free(row->hl);
}

void editorDelRow(int at) {
	if (at < 0 || at >= config.numrows) return;
	// free memory
	editorFreeRow(&config.row[at]);
	// shift right side of array to the left by one
	memmove(&config.row[at], &config.row[at + 1], sizeof(erow) * (config.numrows - at - 1));
	// update editor
	config.numrows--;
	config.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
	if (at < 0 || at > row->size) at = row->size;
	// shift right side of row one to the right
	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at + 1], &row->chars[at], row->size -at + 1);
	row->size++;
	// insert character
	row->chars[at] = c;
	editorUpdateRow(row);
	config.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
	// resize row
	row->chars = realloc(row->chars, row->size + len + 1);
	// append char* to row
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	// update editor
	editorUpdateRow(row);
	config.dirty++;
}

void editorRowDelChar(erow *row, int at) {
	if (at < 0 || at >= row->size) return;
	// move right side of row to the left
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row->size--;
	// update editor
	editorUpdateRow(row);
	config.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
	if (config.cy == config.numrows) {
		editorInsertRow(config.numrows, "", 0);
	}
	editorRowInsertChar(&config.row[config.cy], config.cx, c);
	config.cx++;
}

void editorInsertNewLine() {
	if (config.cx == 0) {
		editorInsertRow(config.cy, "", 0);
	} else {
		erow *row = &config.row[config.cy];
		editorInsertRow(config.cy + 1, &row->chars[config.cx], row->size - config.cx);
		row = &config.row[config.cy];
		row->size = config.cx;
		row->chars[row->size] = '\0';
		editorUpdateRow(row);
	}
	config.cy++;
	config.cx = 0;
}

void editorDelChar() {
	if (config.cy == config.numrows) return;
	if (config.cx == 0 && config.cy == 0) return;

	erow *row = &config.row[config.cy];
	if (config.cx > 0) {
		// delete previous char
		editorRowDelChar(row, config.cx - 1);
		config.cx--;
	} else {
		// delete newline
		config.cx = config.row[config.cy - 1].size;
		editorRowAppendString(&config.row[config.cy - 1], row->chars, row->size);
		editorDelRow(config.cy);
		config.cy--;
	}
}

/*** file I/O ***/

char *editorRowsToString(int *buflen) {
	int totlen = 0;
	int j;
	for (j = 0; j < config.numrows; j++)
		totlen += config.row[j].size + 1;
	*buflen = totlen;

	char *buf = malloc(totlen);
	char *p = buf;
	for (j = 0; j < config.numrows; j++) {
		memcpy(p, config.row[j].chars, config.row[j].size);
		p += config.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

void editorOpen(char* filename) {
	free(config.filename);
	config.filename = strdup(filename);

	editorSelectSyntaxHighlight();

	FILE *fp = fopen(filename, "r");
	if (!fp) die("fopen");

	char *line = NULL; 
	size_t linecap = 0;
	ssize_t linelen;
	// add lines from file to rows
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
							line[linelen - 1] == '\r'))
			linelen--;
		editorInsertRow(config.numrows, line, linelen);
	}
	free(line);
	fclose(fp);
	config.dirty = 0;
}

void editorSave() {
	if (config.filename == NULL) {
		config.filename = editorPrompt("Save as: %s_ (ESC to cancel)", NULL);
		if (config.filename == NULL) {
			editorSetMessage("Save aborted");
			return;
		}
		editorSelectSyntaxHighlight();
	}

	int len;
	char *buf = editorRowsToString(&len);

	int fd = open(config.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1) {
		if (ftruncate(fd, len) != -1) {
			if (write(fd, buf, len) == len) {
				close(fd);
				free(buf);
				config.dirty = 0;
				editorSetMessage("%d bytes written to disk", len);
				return;
			}
		}
		close(fd);
	}

	free(buf);
	editorSetMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char *query, int key) {
	static int last_match = -1;
	static int direction = 1;

	static int saved_hl_line;
	static char *saved_hl = NULL;

	if (saved_hl) {
		memcpy(config.row[saved_hl_line].hl, saved_hl, config.row[saved_hl_line].rsize);
		free(saved_hl);
		saved_hl = NULL;
	}

	if (key == '\r' || key == '\x1b') {
		last_match = -1;
		direction = 1;
		return;
	} else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
		direction = 1;
	} else if (key == ARROW_LEFT || key == ARROW_UP) {
		direction = -1;
	} else {
		last_match = -1;
		direction = 1;
	}

	if (last_match == -1) direction = 1;
	int current = last_match;

	// search rows for term
	int i;
	for (i = 0; i < config.numrows; i++) {
		current += direction;
		if (current == -1) current = config.numrows - 1;
		else if (current == config.numrows) current = 0;

		erow *row = &config.row[current];
		char *match = strstr(row->render, query);
		if (match) {
			last_match = current;
			config.cy = current;
			config.cx = editorRowRxToCx(row, match - row->render);
			config.rowoff = config.numrows;

			saved_hl_line = current;
			saved_hl = malloc(row->rsize);
			memcpy(saved_hl, row->hl, row->rsize);
			memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
			break;
		}
	}
}

void editorFind() { 
	int saved_cx = config.cx;
	int saved_cy = config.cy;
	int saved_coloff = config.coloff;
	int saved_rowoff = config.rowoff;

	// prompt user for search term
	char *query = editorPrompt("Search: %s_ (ESC to cancel)", editorFindCallback);
	if (query) {
		free(query);
	} else {
		config.cx = saved_cx;
		config.cy = saved_cy;
		config.rowoff = saved_rowoff;
		config.coloff = saved_coloff;
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
			char *c = &config.row[filerow].render[config.coloff];
			unsigned char *hl = &config.row[filerow].hl[config.coloff];
			int current_color = -1;
			int j;
			for (j = 0; j < len; j++) {
				if (hl[j] == HL_NORMAL) {
					if (current_color != -1) {
						appendToBuffer(ab, "\x1b[39m", 5);
						current_color = -1;
					}
					appendToBuffer(ab, &c[j], 1);
				} else {
					int color = editorSyntaxToColor(hl[j]);
					if (color != current_color) {
						current_color = color;
						char buf[16];
						int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
						appendToBuffer(ab, buf, clen);
					}
					appendToBuffer(ab, &c[j], 1);
				}
			}
			// NOTE: 39m is default text
			appendToBuffer(ab, "\x1b[39m", 5);
		}

		appendToBuffer(ab, "\x1b[K", 3);
		appendToBuffer(ab, "\r\n", 2);
	}
}

void editorDrawStatusBar(struct appendbuf *ab) {
	// invert colors
	appendToBuffer(ab, "\x1b[7m", 4);
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
		config.filename ? config.filename : "[No Name]", config.numrows,
		config.dirty ? "(modified)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
		config.syntax ? config.syntax->filetype : "no ft", config.cy + 1, config.numrows);
	if (len > config.screencols) len = config.screencols;
	appendToBuffer(ab, status, len);
	while (len < config.screencols) {
		if (config.screencols - len == rlen) {
			appendToBuffer(ab, rstatus, rlen);
			break;
		} else {
			appendToBuffer(ab, " ", 1);
			len++;
		}
	}
	// de-invert colors
	appendToBuffer(ab, "\x1b[m", 3);
	appendToBuffer(ab, "\r\n" , 2);
}

void editorDrawMessageBar(struct appendbuf *ab) {
	appendToBuffer(ab, "\x1b[K", 3);
	int msglen = strlen(config.statusmsg);
	if (msglen > config.screencols) msglen = config.screencols;
	// show message for 5 seconds
	if (msglen && time(NULL) - config.statusmsg_time < 5)
		appendToBuffer(ab, config.statusmsg, msglen);
}

void editorRefreshScreen() {
	editorScroll();

	struct appendbuf ab = ABUF_INIT;

	// hide cursor and move cursor top left
	appendToBuffer(&ab, "\x1b[?25l", 6);
	appendToBuffer(&ab, "\x1b[H", 3);

	// draw
	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);
	
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

void editorSetMessage(const char *fmt, ...) {
	// v stands for variadic function (multiple arguments)
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(config.statusmsg, sizeof(config.statusmsg), fmt, ap);
	va_end(ap);
	config.statusmsg_time = time(NULL);
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
	size_t bufsize = 128;
	char *buf = malloc(bufsize);

	size_t buflen = 0;
	buf[0] = '\0';

	while (1) {
		editorSetMessage(prompt, buf);
		editorRefreshScreen();

		int c = editorReadKey();
		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
			if (buflen != 0) buf[--buflen] = '\0';
		} else if (c == '\x1b') {
			editorSetMessage("");
			if (callback) callback(buf, c);
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (buflen != 0) {
				editorSetMessage("");
				if (callback) callback(buf, c);
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
	
		if (callback) callback(buf, c);
	}
}
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
	static int quit_times = KILO_QUIT_TIMES;

	int c = editorReadKey();

	switch (c) {
		case '\r':
			editorInsertNewLine();
			break;
		// Exit key
		case CTRL_KEY('q'):
			if (config.dirty && quit_times > 0) {
				editorSetMessage("WARNING! File has unsaved changes. "
					"Press CTRL-Q %d more times to quit.", quit_times);
				quit_times--;
				return;
			}
			// Clear Screen
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
		case CTRL_KEY('s'):
			editorSave();
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
			if (config.cy < config.numrows) 
				config.cx = config.row[config.cy].size;
			break;
		case CTRL_KEY('f'):
			editorFind();
			break;
		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
			editorDelChar();
			break;
		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP) {
					config.cy = config.rowoff;
				} else if (c == PAGE_DOWN) {
					config.cy = config.rowoff + config.screenrows - 1;
				}

				int times = config.screenrows;
				while (times--) {
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
				}
			}
			break;
		case CTRL_KEY('l'):
		case '\x1b':
			break;

		default:
			editorInsertChar(c);
			break;
	}

	quit_times = KILO_QUIT_TIMES;
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
	config.dirty = 0;
	config.filename = NULL;
	config.statusmsg[0] = '\0';
	config.statusmsg_time = 0;
	config.syntax = NULL;

	if (getWindowSize(&config.screenrows, &config.screencols) == -1) 
		die("getWindowSize");

	// decrementing screenrows leaves a row for the status bar
	config.screenrows -= 2;
}

int main(int argc, char *argv[]) {
	enableRawMode();
	initEditor();
	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	editorSetMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

	// runtime loop
	while(1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}
	return 0; 
}

