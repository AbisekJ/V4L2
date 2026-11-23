CC = gcc

CFLAGS = -Wall -Wextra -Werror -std=c11

TARGET = camera_app

SOURCES = main.c camera.c

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET)

clean:
	rm -f $(TARGET)
