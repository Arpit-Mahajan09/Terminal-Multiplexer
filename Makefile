CC     = gcc
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE
LIBS   = -lnotcurses -lnotcurses-core

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin

CLIENT_TARGET = cronos
SERVER_TARGET = cronos-server
CLIENT_SRCS   = client.c pane.c keymap.c guide.c parser.c window.c session_ui.c

.PHONY: all clean install uninstall

all: $(CLIENT_TARGET) $(SERVER_TARGET)

$(CLIENT_TARGET): $(CLIENT_SRCS) headers/cronos.h
	$(CC) $(CFLAGS) $(CLIENT_SRCS) -o $(CLIENT_TARGET) $(LIBS)

$(SERVER_TARGET): server.c headers/cronos.h
	$(CC) $(CFLAGS) server.c -o $(SERVER_TARGET) $(LIBS)

clean:
	rm -f $(CLIENT_TARGET) $(SERVER_TARGET)

install: all
	install -m 755 $(CLIENT_TARGET) $(BINDIR)/$(CLIENT_TARGET)
	install -m 755 $(SERVER_TARGET) $(BINDIR)/$(SERVER_TARGET)

uninstall:
	rm -f $(BINDIR)/$(CLIENT_TARGET) $(BINDIR)/$(SERVER_TARGET)