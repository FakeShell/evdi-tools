TARGET = evdi_viewer
SRC = evdi_viewer.c

CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
LDFLAGS = -levdi $(shell pkg-config --libs gtk+-3.0)

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
