CC = gcc
CFLAGS = -std=c89 -ansi -pedantic -pedantic-errors -Wall -g

all: ds peer

ds: objs/ds.o objs/common_utils.o objs/comm.o objs/commandline.o objs/data.o objs/graph.o
	$(CC) $(CFLAGS) $^ -o $@

peer: objs/peer.o objs/common_utils.o objs/comm.o objs/commandline.o objs/data.o objs/graph.o
	$(CC) $(CFLAGS) $^ -o $@

objs/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

objs/:
	mkdir objs

clean:
	rm -rf objs
	rm ds
	rm peer


$(info $(shell mkdir -p objs))
