

SRC =	flac/shndec.c  flac/main.c  flac/decoder.c  buffer.c alsa.c main.c linux_main.c  		\
	ape/entropy.c  ape/filter-pre.c  ape/parser.c   ape/decoder.c  ape/main.c  ape/predictor.c	

SRCX =	tinyxml/tinyxml2.cpp  tinyxml/xmlparser.cpp

OBJ = $(SRC:.c=.o)
OBJX = $(SRCX:.cpp=.o)

CFLAGS = -g -Wall -Wno-unused-variable -Wno-unused-function -DBUILD_STANDALONE -finline-functions -I. -Iinclude
CXXFLAGS = $(CFLAGS)

all = flacplay

all: $(OBJ) $(OBJX)
	g++ $(CFLAGS) $(OBJ) $(OBJX) -o flacplay -lpthread 
	@find . -name \*.o -exec rm '{}' \;


