CC = gcc 
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE 
LIBS = -lnotcurses -lnotcurses-core

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin

CLIENT_TARGET = cronos
SERVER_TARGET = cronos-server
CLIENT_SRCS = client.c pane.c keymap.c guide.c 

.PHONY: all clean install uninstall

all: $(CLIENT_TARGET) $(SERVER_TARGET)

$(CLIENT_TARGET): $(CLIENT_SRCS) cronos.h
	$(CC) $(CFLAGS) $(CLIENT_SRCS) -o $(CLIENT_TARGET) $(LIBS)

$(SERVER_TARGET): server.c cronos.h
	$(CC) $(CFLAGS) server.c -o $(SERVER_TARGET) $(LIBS)

clean:
	rm -f $(CLIENT_TARGET) $(SERVER_TARGET)

install: all
	@echo "Installing cronos to $(BINDIR)..."
	install -m 755 $(CLIENT_TARGET) $(BINDIR)/$(CLIENT_TARGET)
	install -m 755 $(SERVER_TARGET) $(BINDIR)/$(SERVER_TARGET)
	@echo "Installation complete, Type cronos to run"

uninstall:
	@echo "Removing cronos from $(BINDIR)..."
	rm -f $(BINDIR)/$(CLIENT_TARGET)
	rm -f $(BINDIR)/$(SERVER_TARGET)
	@echo "uninstalled cronos"



