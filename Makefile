# gcc -Wall -g -Iinclude -pthread -c src/shm.c -o src/shm.o
# gcc -Wall -g -Iinclude -pthread src/master.c src/shm.o -o master -lrt -pthread
# gcc -Wall -g -Iinclude -pthread src/player.c src/shm.o -o player -lrt -pthread
# gcc -Wall -g -Iinclude -pthread src/view.c src/shm.o -o view -lrt -pthread
run: all
	./bin/master -v ./bin/view -p ./bin/player ./bin/player

CC=gcc
CFLAGS=-Wall -g -Iinclude -pthread
LDFLAGS=-lrt -pthread

SRC_DIR=src
OBJ_DIR=obj
BIN_DIR=bin

all: $(BIN_DIR)/master $(BIN_DIR)/player $(BIN_DIR)/view

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OBJ_DIR)/shm.o: $(SRC_DIR)/shm.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/master: $(SRC_DIR)/master.c $(OBJ_DIR)/shm.o | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/player: $(SRC_DIR)/player.c $(OBJ_DIR)/shm.o | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/view: $(SRC_DIR)/view.c $(OBJ_DIR)/shm.o | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) master player view

.PHONY: all clean