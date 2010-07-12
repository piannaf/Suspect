CC = gcc
CFLAGS = -Wall -std=gnu99 -pedantic
OBJECTS = suspect.o

all: suspect

suspect: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

debug: $(OBJECTS)
	$(CC) -g $(CFLAGS) -o $@ $^
