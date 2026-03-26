CC	?= gcc
CFLAGS	?= -g -Wall
LDFLAGS	?= -lunvmed

SRCS	= nvme-queue-info.c
TARGET	= nvme-queue-info

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
