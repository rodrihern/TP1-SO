
.DEFAULT_GOAL := all

run: all
	./bin/master -v ./bin/view -p ./bin/player ./bin/player

# Ejecutar el master de cátedra con nuestras view/player
catedra: all
	./master_catedra -v ./bin/view -p ./bin/player ./bin/player

CC=gcc
CFLAGS=-Wall -g -Iinclude -pthread
LDFLAGS=-lrt -pthread
# Selección flexible de ncurses: por defecto -lncurses, se puede overridear
# Ejemplos:
#   make                         # usa -lncurses
#   make NCURSES_LIB=-lncursesw  # usa la wide-char
#   make NCURSES_LIB=            # no linkea ncurses
NCURSES_LIB ?= -lncurses
LDFLAGS += $(NCURSES_LIB)

SRC_DIR=src
OBJ_DIR=obj
BIN_DIR=bin

# Fuerza recompilación completa: limpia y luego compila
all: clean $(BIN_DIR)/master $(BIN_DIR)/player $(BIN_DIR)/view

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

.PHONY: all clean run catedra