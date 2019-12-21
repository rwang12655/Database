CFLAGS = -g3 -Wall -Wextra -Wcast-qual -Wcast-align -g
CFLAGS += -Winline -Wfloat-equal -Wnested-externs
CFLAGS += -std=gnu99 -D_GNU_SOURCE -pthread
EXECS = server client
CC = gcc

.PHONY: all clean

all: $(EXECS)

server: server.c comm.c db.c
	$(CC) $(CFLAGS) $^ -o $@

client: client.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(EXECS)
