# Compiler and flags
CC = gcc
CFLAGS = -Wall
LDFLAGS = -lncurses

# Paths
SRC_DIR = src
BUILD_DIR = build
BIN_DIR = build

NNTM_SRC = $(SRC_DIR)/nntm.c
NNTMD_SRC = $(SRC_DIR)/nntmd.c

NNTM_OBJ = $(BUILD_DIR)/nntm.o
NNTMD_OBJ = $(BUILD_DIR)/nntmd.o

NNTM_BIN = $(BIN_DIR)/nntm
NNTMD_BIN = $(BIN_DIR)/nntmd

# Targets
all: $(NNTM_BIN) $(NNTMD_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(NNTM_OBJ): $(NNTM_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(NNTMD_OBJ): $(NNTMD_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(NNTM_BIN): $(NNTM_OBJ)
	$(CC) $(NNTM_OBJ) $(LDFLAGS) -o $@

$(NNTMD_BIN): $(NNTMD_OBJ)
	$(CC) $(NNTMD_OBJ) $(LDFLAGS) -o $@

clean:
	rm -rf $(BUILD_DIR)

install: $(NNTM_BIN) $(NNTMD_BIN)
	install -Dm755 $(NNTM_BIN) /usr/bin/nntm
	install -Dm755 $(NNTMD_BIN) /usr/bin/nntmd

.PHONY: all clean install

