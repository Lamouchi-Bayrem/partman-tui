CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -D_FORTIFY_SOURCE=2
LDLIBS += -lncursesw
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/sbin

all: partman-tui
partman-tui: src/main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)
check:
	$(CC) $(CFLAGS) -fsyntax-only src/main.c
install: partman-tui
	install -Dm755 partman-tui $(DESTDIR)$(BINDIR)/partman-tui
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/partman-tui
clean:
	rm -f partman-tui
.PHONY: all check install uninstall clean
