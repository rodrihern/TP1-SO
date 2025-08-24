# gcc -Wall -g -Iinclude -pthread -c src/shm.c -o src/shm.o
# gcc -Wall -g -Iinclude -pthread src/master.c src/shm.o -o master -lrt -pthread
# gcc -Wall -g -Iinclude -pthread src/player.c src/shm.o -o player -lrt -pthread
# gcc -Wall -g -Iinclude -pthread src/view.c src/shm.o -o view -lrt -pthread
run: all
	./master -v ./view -p ./player ./player

CC=gcc
CFLAGS=-Wall -g -Iinclude -pthread
LDFLAGS=-lrt -pthread

SRC_DIR=src
OBJ_DIR=obj

all: master player view

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/shm.o: $(SRC_DIR)/shm.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

master: $(SRC_DIR)/master.c $(OBJ_DIR)/shm.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

player: $(SRC_DIR)/player.c $(OBJ_DIR)/shm.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

view: $(SRC_DIR)/view.c $(OBJ_DIR)/shm.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR) master player view

.PHONY: all clean