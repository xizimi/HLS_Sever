CXX = g++
CXXFLAGS = -std=c++14 -O2 -Wall -g -I/usr/include/mariadb

TARGET = server
OBJS = ../code/log/*.cpp ../code/pool/*.cpp ../code/timer/*.cpp \
       ../code/http/*.cpp ../code/server/*.cpp \
       ../code/buffer/*.cpp ../code/main.cpp

all: $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o ../bin/$(TARGET)  -pthread -lmariadbclient

clean:
	rm -rf ../bin/$(OBJS) $(TARGET)




