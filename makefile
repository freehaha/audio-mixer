all: compile

test: compile
	./out -f ./mixer

debug: main.c
	gcc -DDEBUG -g -o out `pkg-config --cflags --libs gstreamer-1.0` main.c
	./out -f ./mixer 2>&1 | tee log

compile: main.c
	gcc-8 -O2 -o out `pkg-config --cflags --libs gstreamer-1.0` main.c

discord: discord.c
	gcc -o discord `pkg-config --cflags --libs gstreamer-1.0` discord.c

