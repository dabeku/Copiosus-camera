CC = gcc

DEPS =  cop_status_code.h	\
	cop_network.h	\
	cop_list.h

# use pkg-config for getting CFLAGS and LDLIBS
FFMPEG_LIBS=    libavdevice                        \
                libavformat                        \
                libavfilter                        \
                libavcodec                         \
                libswresample                      \
                libswscale                         \
                libavutil                          \

CFLAGS += -Wall -g
CFLAGS := $(shell pkg-config --cflags $(FFMPEG_LIBS)) $(shell sdl2-config --cflags) $(CFLAGS)
LDLIBS := $(shell pkg-config --libs $(FFMPEG_LIBS)) $(shell sdl2-config --libs) $(LDLIBS)
	
COPIOSUS = 	cop_sender

OBJS = 		cop_sender.o	\
		cop_network.o		\
		cop_utility.o		\
		cop_list.o

all: clean cop_utility.o cop_network.o cop_list.o cop_sender

cop_utility.o: cop_utility.c cop_utility.h
	$(CC) $(CFLAGS) -c cop_utility.c

cop_network.o: cop_network.c cop_network.h
	$(CC) $(CFLAGS) -c cop_network.c

cop_list.o: cop_list.c cop_list.h
	$(CC) $(CFLAGS) -c cop_list.c

cop_sender: cop_sender.o
	gcc cop_sender.o cop_utility.o cop_network.o cop_list.o -o cop_sender $(CFLAGS) $(LDLIBS)

clean-test:
	$(RM) test.*

clean: clean-test
	$(RM) $(COPIOSUS) $(OBJS)
