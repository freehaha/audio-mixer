all: compile

test: compile
	./out

debug: compile
	GST_DEBUG=4 ./out

compile: main.c
	gcc -DDEBUG -g -o out `pkg-config --cflags --libs gstreamer-1.0` main.c
	# gcc -O2 -o out `pkg-config --cflags --libs gstreamer-1.0` main.c

discord: discord.c
	gcc -o discord `pkg-config --cflags --libs gstreamer-1.0` discord.c

