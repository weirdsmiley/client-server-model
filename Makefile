CFLAGS := -g -Wall -lpthread -Wformat-truncation=0
SRV := src/server.c
CLI := src/client.c

$(shell mkdir -p build)

.PHONY: build/srv
build/srv: $(SRV)
	$(CC) $(CFLAGS) $< -o $@

.PHONY: build/cli
build/cli: $(CLI)
	$(CC) $(CFLAGS) $< -o $@

.DEFAULT_GOAL := all
all: build/srv build/cli

clean:
	rm -f build/srv build/cli
	rmdir build/
