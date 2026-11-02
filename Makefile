CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  += -lasound

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

BIN := audio_streamer
SRC := audio_streamer.c

.PHONY: all install uninstall clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: $(BIN)
	install -D -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	rm -f $(BIN)
