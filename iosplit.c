#include <curses.h>
#include <locale.h>
#include <ctype.h>
#include <err.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#ifdef HAVE_PTY_H
#include <pty.h>
#else
#include <util.h>
#endif

#ifndef INFTIM
#define INFTIM -1
#endif

#include <poll.h>
#include <string.h>
#include <termios.h>

struct view {
	struct row *top;
	size_t top_linenum;
};

enum {
	OUTPUT_CURSOR,
	INPUT_CURSOR
};

struct cursor {
	struct row *row;
	size_t col;
	int type;
};

struct buffer {
	struct cursor input;
	struct cursor output;
	struct row *first;
	struct view view;
};

#define CMD_ROW (1 << 0)

struct row {
	char *text;
	int flags;
	size_t text_alloc;
	size_t text_len;
	size_t prompt_len;
	struct row *prev;
	struct row *next;
	struct buffer *buffer;
};

#define ROW_INITIAL_ALLOC (40)

void insert_text(struct cursor *, struct cursor *, char *, int);
void draw_buffer(WINDOW *, struct buffer *, int);
void init_buffer(struct buffer *);
struct row *add_row(struct buffer *, struct row *);
int is_trailing_cursor(struct cursor *, struct cursor *);
struct row *find_row_flags(struct row *, int);
void remove_row(struct row *);
void clear_to(struct row *, struct row *);
void set_cursor(struct cursor *, struct row *, size_t);
size_t nlines(struct row *, struct row *);

size_t
nlines(struct row *begin, struct row *end)
{
	struct row *np;
	size_t n;

	n = 0;
	for (np = begin; np != NULL && np != end; np = np->next)
		n++;

	return n;
}

int
is_trailing_cursor(struct cursor *candidate, struct cursor *reference)
{
	if (candidate->row == reference->row &&
	    candidate->col >= reference->col)
		return 1;

	return 0;
}

struct row *
find_row_flags(struct row *np, int flags)
{
	for (; np != NULL; np = np->next)
		if (np->flags & flags)
			return np;

	return NULL;
}

void
remove_row(struct row *np)
{
	if (np == np->buffer->first && np->buffer->first->next == NULL)
		return;

	if (np->prev)
		np->prev->next = np->next;
	if (np->next)
		np->next->prev = np->prev;
	if (np == np->buffer->first) {
		if (np->buffer->view.top == np)
			np->buffer->view.top = np->next;
		np->buffer->first = np->next;
	}
	free(np);
}

void
clear_to(struct row *np, struct row *last)
{
	struct row *next;

	for (; np != NULL && np != last; np = next) {
		next = np->next;
		remove_row(np);
	}
}

void
insert_text(struct cursor *cursor, struct cursor *other, char *text, int rows)
{
	struct row *row;

	assert(cursor != NULL);
	assert(cursor->row != NULL);

	while (*text != '\0') {
		if (*text == '\r') {
			text++;
			continue;
		}
		if (*text == '\n') {
			if (cursor->type == OUTPUT_CURSOR)
				cursor->row->prompt_len = 0;

			row = add_row(cursor->row->buffer, cursor->row);

			if (is_trailing_cursor(other, cursor)) {
				cursor->row = other->row = row;
				cursor->col = other->col = 0;
			} else {
				cursor->row = row;
				cursor->col = 0;
			}
			text++;

			if (nlines(cursor->row->buffer->view.top,
			    row) >= rows)
				cursor->row->buffer->view.top =
				    cursor->row->buffer->view.top->next;

			continue;
		}

		if (cursor->row->text_len == cursor->row->text_alloc) {
			if (cursor->row->text_alloc == 0)
				cursor->row->text_alloc = ROW_INITIAL_ALLOC;
			else
				cursor->row->text_alloc *= 2;
			cursor->row->text = realloc(cursor->row->text,
			    cursor->row->text_alloc * sizeof(char));
			if (cursor->row->text == NULL)
				err(1, "realloc");
		}
		if (cursor->col > cursor->row->text_len)
			cursor->col = cursor->row->text_len;

		if (is_trailing_cursor(other, cursor))
			other->col++;

		cursor->row->text_len++;
		memmove(&cursor->row->text[cursor->col+1],
		    &cursor->row->text[cursor->col],
		    cursor->row->text_len - cursor->col);
		cursor->row->text[cursor->col++] = *text++;

		if (cursor->type == OUTPUT_CURSOR)
			cursor->row->prompt_len = cursor->col;
	}
}

void
draw_buffer(WINDOW *win, struct buffer *buffer, int rows)
{
	struct row *np;
	size_t linenum, i, row;

	assert(buffer != NULL);

	linenum = buffer->view.top_linenum;
	row = 0;
	for (np = buffer->view.top; np != NULL && row < rows;
	    np = np->next, rows++) {
		wmove(win, linenum, 0);
		wclrtoeol(win);
#ifdef WANT_ROW_INFO
		wattron(win, A_REVERSE);
		mvwprintw(win, linenum, 0, "%3d %3d/%3d %3d %c",
		    linenum+1, np->text_len,
		    np->text_alloc, np->prompt_len,
		    np->flags & CMD_ROW ? 'c' : '-');
		wattroff(win, A_REVERSE);
		waddch(win, ' ');
#endif
		for (i = 0; i < np->text_len; i++) {
			if (np->text[i] == '\r')
				continue;
			if (buffer->input.row == np && buffer->input.col == i)
				wattron(win, COLOR_PAIR(1));
			if (buffer->output.row == np && buffer->output.col == i)
				wattron(win, COLOR_PAIR(2));
			waddch(win, np->text[i]);

			if (buffer->input.row == np && buffer->input.col == i)
				wattroff(win, COLOR_PAIR(1));
			if (buffer->output.row == np && buffer->output.col == i)
				wattroff(win, COLOR_PAIR(2));
		}
		if (buffer->input.row == np && buffer->input.col == i)
			wattron(win, COLOR_PAIR(1));
		else if (buffer->output.row == np && buffer->output.col == i)
			wattron(win, COLOR_PAIR(2));

		waddch(win, ' ');

		if (buffer->input.row == np && buffer->input.col == i)
			wattroff(win, COLOR_PAIR(1));
		else if (buffer->output.row == np && buffer->output.col == i)
			wattroff(win, COLOR_PAIR(2));

		linenum++;
	}
	wclrtobot(win);
	wrefresh(win);
}

void
set_cursor(struct cursor *cursor, struct row *row, size_t col)
{
	cursor->col = col;
	cursor->row = row;
}

void
init_buffer(struct buffer *buffer)
{
	assert(buffer != NULL);

	buffer->first = add_row(buffer, NULL);
	set_cursor(&buffer->input, buffer->first, 0);
	set_cursor(&buffer->output, buffer->first, 0);
	buffer->input.type = INPUT_CURSOR;
	buffer->output.type = OUTPUT_CURSOR;

	buffer->view.top = buffer->first;
	buffer->view.top_linenum = 0;
}

struct row *
add_row(struct buffer *buffer, struct row *after)
{
	struct row *row;

	assert(buffer != NULL);

	row = calloc(1, sizeof(struct row));
	if (row == NULL)
		err(1, "calloc");
	row->buffer = buffer;

	if (after == NULL) {
		row->next = buffer->first;
		if (buffer->first != NULL)
			buffer->first->prev = row;
		buffer->first = row;
	} else {
		if (after->next) {
			after->next->prev = row;
			row->next = after->next;
		}
		after->next = row;
		row->prev = after;
	}

	return row;
}

int
main(int argc, char *argv[])
{
	WINDOW *win, *owin;
#ifdef WANT_BOXES
	WINDOW *iwin, *swin;
	int col = 0;
#endif
	int ch;
	char buf[4096 + 1], *p, ibuf[4096 + 1];
	pid_t pid;
	int fd;
	int n;
	struct pollfd pfd[2];
	int nready;
	static struct buffer buffer;
	struct row *row;
	int rows, cols;

	init_buffer(&buffer);

	p = buf;
	*p = '\0';

	setlocale(LC_ALL, "");

	win = initscr();

	start_color();

	init_pair(1, COLOR_WHITE, COLOR_GREEN);
	init_pair(2, COLOR_WHITE, COLOR_RED);

#ifdef WANT_BOXES
	owin = newwin(getmaxy(win) - 2, getmaxx(win), 0, 0);
	swin = newwin(1, getmaxx(win), getmaxy(win) - 2, 0);
	iwin = newwin(1, getmaxx(win), getmaxy(win) - 1, 0);

	idlok(iwin, true);
	idlok(owin, true);
	keypad(iwin, true);
	keypad(win, true);
	keypad(owin, true);

	whline(swin, '-', getmaxx(win));

	wrefresh(win);
	wrefresh(owin);
	wrefresh(iwin);
	wrefresh(swin);
#else
	owin = newwin(getmaxy(win), getmaxx(win), 0, 0);

	idlok(owin, true);
	keypad(owin, true);
	keypad(win, true);
	curs_set(0);
	wrefresh(win);
	wrefresh(owin);
#endif

	getmaxyx(owin, rows, cols);

	raw();
	noecho();

	setenv("TERM", "dumb", 1);

	pid = forkpty(&fd, NULL, NULL, NULL);
	if (pid < 0)
		err(1, "forkpty");
	if (pid == 0)
		execl("/bin/sh", "/bin/sh", NULL);

#ifdef HAVE_PLEDGE
	if (pledge("stdio tty", NULL) != 0)
		err(1, "pledge");
#endif

	pfd[0].fd = STDIN_FILENO;
	pfd[0].events = POLLIN;

	pfd[1].fd = fd;
	pfd[1].events = POLLIN;

	while (1) {
		nready = poll(pfd, sizeof(pfd) / sizeof(pfd[0]), INFTIM);
		if (nready == -1)
			err(1, "poll");
		if (pfd[0].revents & (POLLIN|POLLHUP)) {
			ch = wgetch(win);

			/*
			 * Write control commands immediately.
			 */
			if (iscntrl(ch) && ch != '\t' && ch != '\n' &&
			    isascii(ch)) {
				write(fd, &ch, 1);
				continue;
			}

			switch (ch) {
			case '\n':
				if (buffer.input.row->text) {
					row = buffer.input.row;
					write(fd,
					    &row->text[row->prompt_len],
					    row->text_len - row->prompt_len);
					write(fd, "\n", 1);
					free(row->text);
					row->flags |= CMD_ROW;
					row->text = NULL;
					row->text_alloc = 0;
					row->text_len = 0;
					row->prompt_len = 0;
					buffer.input.col = 0;
				}
				buffer.output.row = buffer.input.row;
				buffer.output.col = buffer.input.col;

				clear_to(row->next, find_row_flags(row->next,
				    CMD_ROW));
				break;
			case KEY_UP:
				row = buffer.input.row;
				if (row == buffer.view.top && row->prev)
					buffer.view.top = row->prev;
				row = buffer.input.row = row->prev;
				if (row == NULL)
					row = buffer.input.row = buffer.first;
				if (buffer.input.col >= row->text_len)
					buffer.input.col = row->text_len;
				draw_buffer(owin, &buffer, rows);
				break;
			case KEY_DOWN:
				row = buffer.input.row;
				if (row->next != NULL)
					row = buffer.input.row = row->next;
				if (buffer.input.col >= row->text_len)
					buffer.input.col = row->text_len;

				if (nlines(row->buffer->view.top,
				    row) >= rows)
					row->buffer->view.top =
					    row->buffer->view.top->next;

				draw_buffer(owin, &buffer, rows);
				break;
			case KEY_LEFT:
				if (buffer.input.col > 0)
					buffer.input.col--;
				draw_buffer(owin, &buffer, rows);
				break;
			case KEY_RIGHT:
				row = buffer.input.row;
				if (buffer.input.col < row->text_len)
					buffer.input.col++;
				draw_buffer(owin, &buffer, rows);
				break;
			case KEY_BACKSPACE:
				row = buffer.input.row;
				if (buffer.input.col > 0) {
					row->text_len--;

					if (buffer.input.col <= row->prompt_len)
					    row->prompt_len--;

					if (is_trailing_cursor(&buffer.output,
					    &buffer.input))
						buffer.output.col--;
					buffer.input.col--;

					if (row->text != NULL)
						row->text[row->text_len] = '\0';
				}
				draw_buffer(owin, &buffer, rows);
				break;
			default:
				ibuf[0] = ch;
				ibuf[1] = '\0';
				insert_text(&buffer.input, &buffer.output,
				    ibuf, rows);
				draw_buffer(owin, &buffer, rows);
				break;
			}
#ifdef WANT_BOXES
			mvwprintw(swin, 0, 72-1, "%3d", col + 1);
			wrefresh(swin);
			wrefresh(iwin);
#endif
		}
		if (pfd[1].revents & (POLLIN|POLLHUP)) {
			n = read(fd, ibuf, sizeof(ibuf));
			if (n > 0) {
				ibuf[n] = '\0';
				insert_text(&buffer.output, &buffer.input,
				    ibuf, rows);
			} else {
				if (n < 0) {
					endwin();
					err(1, "read");
				}
				break;
			}
			draw_buffer(owin, &buffer, rows);
			wrefresh(owin);
#ifdef WANT_BOXES
			wrefresh(iwin);
#endif
		}
	}

	endwin();
	return 0;
}
