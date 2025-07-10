CC = cc
CFLAGS = -Wall -Werror -O2
TARGET = mx3_driver

all: $(TARGET)

$(TARGET): mx3_driver.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
