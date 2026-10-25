CC = gcc
PKG = gtk+-3.0 gio-2.0 gio-unix-2.0 vte-2.91
CFLAGS = $(shell pkg-config --cflags $(PKG)) -Wall -O2 -std=gnu11
LIBS = $(shell pkg-config --libs $(PKG))

PREFIX ?= /usr
BINDIR = $(PREFIX)/bin
APPDIR = $(PREFIX)/share/applications
ICONDIR = $(PREFIX)/share/icons/hicolor/256x256/apps

SRC = main.c fileview-core.c fileview-fileops.c fileview-actions.c fileview-ui.c \
      sidebar.c thumbnail.c copyjob.c deletejob.c renamejob.c compressjob.c \
      infopanel.c terminal.c iconchooser.c undo.c

OBJ = $(SRC:.c=.o)
BIN = gtkfm

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $(BIN) $(LIBS)

%.o: %.c app.h fileview.h sidebar.h thumbnail.h copyjob.h deletejob.h renamejob.h compressjob.h infopanel.h terminal.h undo.h
	$(CC) $(CFLAGS) -c $< -o $@

fileview-core.o fileview-fileops.o fileview-actions.o fileview-ui.o compressjob.o undo.o: fileview_internal.h
fileview-fileops.o: iconchooser.h

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

	install -d $(DESTDIR)$(APPDIR)
	install -m644 gtkfm.desktop $(DESTDIR)$(APPDIR)/gtkfm.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(APPDIR)/gtkfm.desktop

clean:
	rm -f $(OBJ) $(BIN)

run: $(BIN)
	./$(BIN)

.PHONY: all clean run install uninstall
