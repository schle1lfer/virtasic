CC = gcc
CXX = g++
LD = gcc
SRC = main.c
OBJ = $(SRC:.c=.o)
INCLUDES = -I/usr/include/libnl3
CFLAGS = -Wall -Wextra
CXXFLAGS = -Wall -Wextra
LDFLAGS =

# sudo apt-get install libnl-3-dev libnl-genl-3-dev libnl-route-3-dev
STATIC_LIBS =
DYNAMIC_LIBS = -lpthread -ldl -lnl-3 -lnl-genl-3 -lnl-route-3

TARGET_DAEMON = virtasic

all: $(TARGET_DAEMON)

$(TARGET_DAEMON): $(OBJ)
	$(LD) -o $@ $^ $(LDFLAGS) $(STATIC_LIBS) $(DYNAMIC_LIBS)

%.o: %.c
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(INCLUDES) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET_DAEMON)

distclean: clean

.PHONY: all clean distclean