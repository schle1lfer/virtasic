CC = gcc
CXX = g++
LD = gcc
INCLUDES = -I/usr/include/libnl3
CFLAGS = -Wall -Wextra
CXXFLAGS = -Wall -Wextra
LDFLAGS =

# sudo apt-get install libnl-3-dev libnl-genl-3-dev libnl-route-3-dev
STATIC_LIBS =
DYNAMIC_LIBS = -lpthread -ldl -lnl-3 -lnl-genl-3 -lnl-route-3

TARGET_DAEMON = virtasic
TARGET_TEST   = test_vlan

DAEMON_OBJS = main.o vlan_api.o bridge_netlink.o
TEST_OBJS   = test_vlan.o vlan_api.o bridge_netlink.o

all: $(TARGET_DAEMON) $(TARGET_TEST)

$(TARGET_DAEMON): $(DAEMON_OBJS)
	$(LD) -o $@ $^ $(LDFLAGS) $(STATIC_LIBS) $(DYNAMIC_LIBS)

$(TARGET_TEST): test_vlan.o vlan_api.o
	$(LD) -o $@ $^ $(LDFLAGS) $(STATIC_LIBS) $(DYNAMIC_LIBS)

%.o: %.c
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(INCLUDES) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(DAEMON_OBJS) $(TEST_OBJS) $(TARGET_DAEMON) $(TARGET_TEST)

distclean: clean

.PHONY: all clean distclean
