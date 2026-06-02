CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 \
           -D_POSIX_C_SOURCE=200809L \
           -Wno-unused-parameter
LDFLAGS = -lpthread -lm
TARGET  = telemetry_forwarder
SRC     = src/telemetry_forwarder.c

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Unit test build (links test runner against the same source with TEST macro)
test: tests/test_decoder
	./tests/test_decoder

tests/test_decoder: tests/test_decoder.c $(SRC)
	$(CC) $(CFLAGS) -DUNIT_TEST -o $@ tests/test_decoder.c $(LDFLAGS)

clean:
	rm -f $(TARGET) tests/test_decoder
