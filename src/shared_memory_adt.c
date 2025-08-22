#include "shared_memory_adt.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    int     fd;           // file descriptor de shm_open
    size_t  size;         // tamaño en bytes
    void*   addr;         // puntero al mmap
    bool    owner;        // true si este proceso la creó
    char    name[64];     // nombre POSIX, ej "/game_state"
} shared_memory_cdt;
