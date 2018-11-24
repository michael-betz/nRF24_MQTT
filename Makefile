#############################################################################
#
# Makefile for Raspberry Pi
#
#############################################################################

TARGET = nRFserver
LIBS = -lbcm2835 -lmosquitto
CC = gcc
# The recommended compiler flags for the Raspberry Pi
CFLAGS = -Wall -std=gnu99 -Ofast -mfpu=vfp -mfloat-abi=hard -march=armv6zk -mtune=arm1176jzf-s

all: $(TARGET)

OBJECTS = $(patsubst %.c, %.o, $(wildcard *.c))
HEADERS = $(wildcard *.h)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -Wall $(LIBS) -o $@

clean:
	-rm -f *.o
	-rm -f $(TARGET)
	
.INTERMEDIATE: $(OBJECTS)
.PHONY: clean all
