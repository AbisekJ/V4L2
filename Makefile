CC = gcc

CFLAGS = -Wall -Wextra -Werror -std=c11 -g
CPPFLAGS += $(shell pkg-config --cflags libudev)
LDLIBS += $(shell pkg-config --libs libudev)

TARGET = camera_app
SOURCES = main.c camera.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
