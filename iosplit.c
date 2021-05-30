#include <curses.h>
#include <locale.h>
#include <ctype.h>
#include <err.h>
#include <unistd.h>
#include <stdlib.h>

#ifdef __OpenBSD__
#include <util.h>
#else
#include <pty.h>
#endif

#ifndef INFTIM
#define INFTIM -1
#endif

#include <poll.h>
#include <string.h>
#include <termios.h>

int
recalc_col(const char *s)
{
	int col;

	col = 0;
	while (*s != '\0') {
		if (*s == '\t')
			col += (8 - (col % 8));
		else
			col++;
		s++;
	}

	return col;
}

int
main(int argc, char *argv[])
{
	WINDOW *win, *owin, *iwin, *swin;
	int ch;
	char buf[4096 + 1], *p, ibuf[4096 + 1], *ip;
	pid_t pid;
	int fd;
	int n;
	struct pollfd pfd[2];
	int nready;
	unsigned char c;
	int nlines = 0;
	int col = 0;

	p = buf;
	*p = '\0';

	setlocale(LC_ALL, "");

	win = initscr();

	owin = newwin(getmaxy(win) - 2, getmaxx(win), 0, 0);
	swin = newwin(1, getmaxx(win), getmaxy(win) - 2, 0);
	iwin = newwin(1, getmaxx(win), getmaxy(win) - 1, 0);

	scrollok(iwin, true);
	scrollok(owin, true);
	idlok(iwin, true);
	idlok(owin, true);
	keypad(iwin, true);
	keypad(win, true);

	whline(swin, '-', getmaxx(win));

	wrefresh(win);
	wrefresh(owin);
	wrefresh(iwin);
	wrefresh(swin);

	raw();
	noecho();

	setenv("TERM", "dumb", 1);

	pid = forkpty(&fd, NULL, NULL, NULL);
	if (pid < 0)
		err(1, "forkpty");
	if (pid == 0)
		execl("/bin/sh", "/bin/sh", NULL);

	pfd[0].fd = STDIN_FILENO;
	pfd[0].events = POLLIN;

	pfd[1].fd = fd;
	pfd[1].events = POLLIN;

	while (1) {
		nready = poll(pfd, sizeof(pfd) / sizeof(pfd[0]), INFTIM);
		if (nready == -1)
			err(1, "poll");
		if (pfd[0].revents & (POLLIN|POLLHUP)) {
			ch = wgetch(iwin);

			/*
			 * Write control commands immediately.
			 */
			if (iscntrl(ch) && ch != '\t' && ch != '\n' &&
			    isascii(ch)) {
				write(fd, &ch, 1);
				continue;
			}
			switch (ch) {
			case '\t':
				*p++ = '\t';
				*p = '\0';
				col = recalc_col(buf);
				if (col > 79) {
					*--p = '\0';
					col = recalc_col(buf);
				}
				wmove(iwin, 0, col);
				wclrtoeol(iwin);
				break;
			case '\n':
				*p++ = (unsigned char) ch;
				*p = '\0';

				n = write(fd, buf, strlen(buf));
				p = buf;
				nlines = 1;
				waddch(iwin, '\n');
				col = 0;
				break;
			case KEY_BACKSPACE:
				if (col > 0 && p > buf) {
					*--p = '\0';
					col = recalc_col(buf);
					wmove(iwin, 0, col);
					wclrtoeol(iwin);
				}
				break;
			default:
				if (col < 80-1) {
					if (isprint(ch) || iscntrl(ch)) {
						*p++ = (unsigned char) ch;
						*p = '\0';
					}
					col++;
					waddch(iwin, ch);
				}
				break;
			}
			mvwprintw(swin, 0, 72-1, "%3d", col + 1);
			wrefresh(swin);
			wrefresh(iwin);
		}
		if (pfd[1].revents & (POLLIN|POLLHUP)) {
			n = read(fd, ibuf, sizeof(ibuf));
			if (n > 0) {
				ibuf[n] = '\0';
				ip = ibuf;

				if (nlines)
					wattron(owin, A_REVERSE);

				while ((c = *ip++) != '\0') {
					if (c == '\r') {
						if (nlines) {
							nlines = 0;
							wattroff(owin,
							    A_REVERSE);
						}
						continue;
					}
					waddch(owin, c);
				}
			} else {
				if (n < 0) {
					endwin();
					err(1, "read");
				}
				break;
			}
			wrefresh(owin);
			wrefresh(iwin);
		}
	}

	endwin();
	return 0;
}
