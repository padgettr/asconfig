
PREFIX = /usr/local

all: asconfig

asconfig: asconfig.c
	gcc -Wall -o $@ $^ -lasound `pkg-config --libs --cflags gtk+-3.0`

install: asconfig
	install -D -m 755 asconfig $(PREFIX)/bin/asconfig
