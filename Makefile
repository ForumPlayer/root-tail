COPTS = -g

CFLAGS = -Wall $(COPTS)

LDFLAGS = -L/usr/X11R6/lib -lX11 -lXfixes

PREFIX = $(DESTDIR)/usr
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1
DOCDIR = $(PREFIX)/share/doc/root-tail

SOURCES = root-tail.c

all: root-tail man

root-tail: $(SOURCES) config.h
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

man: root-tail.1.gz

root-tail.1.gz: root-tail.man
	cp root-tail.man root-tail.1
	gzip -f9 root-tail.1

clean:
	rm -f root-tail root-tail.o root-tail.1.gz

install: all
	install -D -o root -g root root-tail $(BINDIR)
	install -D -m 0644 -o root -g root root-tail.1.gz $(MANDIR)
	install -D -m 0644 -o root -g root README $(DOCDIR)
	install -m 0644 -o root -g root Changes $(DOCDIR)

uninstall:
	rm -f $(BINDIR)/root-tail $(MANDIR)/root-tail.1.gz
	rm -f $(DOCDIR)/README $(DOCDIR)/Changes
	rmdir --ignore-fail-on-non-empty $(DOCDIR)

