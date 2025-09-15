// This is a personal academic project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "common.h"
#include "shm.h"


struct shm_cdt {
    char   *name;    // copia del nombre (para unlink)
    int     fd;      // descriptor de archivo devuelto por shm_open
    size_t  size;    // tamaño mapeado actual
    void   *base;    // dirección base del mmap (NULL si no está mapeado)
    bool    owner;   // true si este proceso la creó 
};


static int ensure_size(int fd, size_t sz) { 
    return ftruncate(fd, (off_t)sz); 
}

static void *map_rw(int fd, size_t sz) { 
    void *p = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    return (p == MAP_FAILED) ? NULL : p;
}

static int unmap_if_mapped(struct shm_cdt *r) { 
    if (r->base && r->size) { 
        if (munmap(r->base, r->size) == -1)  
            return -1;
        r->base = NULL;
    }
    return 0;
}

static void free_shm_handle(struct shm_cdt *h) {
    if (h) {
        free(h->name);
        free(h);
    }
}

static int init_game_sync_semaphores(game_sync_t *sync){
    if (sem_init(&sync->view_ready, 1, 0) == -1) 
        return -1; // A
    if (sem_init(&sync->view_done, 1, 0) == -1) 
        return -1; // B
    if (sem_init(&sync->writer_mutex, 1, 1) == -1) 
        return -1; // C
    if (sem_init(&sync->state_mutex, 1, 1) == -1) 
        return -1; // D
    if (sem_init(&sync->reader_count_mutex, 1, 1) == -1) 
        return -1; // E
    sync->reader_count = 0; // F
   
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (sem_init(&sync->player_ready[i], 1, 0) == -1) 
            return -1; // G[i]
    }
    return 0;
}




int shm_region_open(shm_adt *out_handle, const char *name, size_t size_bytes) {
    if (!out_handle || !name || size_bytes == 0) { 
        errno = EINVAL; 
        return -1; 
    } 

    struct shm_cdt *h = calloc(1, sizeof(*h)); 
    if (!h) 
        return -1;

    h->name = strdup(name); 
    if (!h->name) {
        free(h); 
        return -1;
    }

    h->fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0666);

    if (h->fd != -1) { 
        h->owner = true;
        h->size  = size_bytes;
        if (ensure_size(h->fd, size_bytes) == -1) { 
            int e = errno;
            close(h->fd);
            shm_unlink(name);
            free_shm_handle(h);
            errno = e;
            return -1; 
        }
        (void)fchmod(h->fd, 0666);
    } else if (errno == EEXIST) { 
        h->fd = shm_open(name, O_RDWR, 0);

        if (h->fd == -1) {
            int e = errno;
            free_shm_handle(h);
            errno = e;
            return -1;
        }
        struct stat st;
        if (fstat(h->fd, &st) == -1) {
            int e = errno;
            close(h->fd); 
            free_shm_handle(h);
            errno = e;
            return -1;
        }
        h->size  = (size_t)st.st_size; 
        h->owner = false;
    } else { 
        int e = errno;
        free_shm_handle(h);
        errno = e;
        return -1;
    }
    h->base = NULL;
    *out_handle = h;
    return 0;
}

int shm_region_close(shm_adt handle) {
    if (!handle) { 
        errno = EINVAL; 
        return -1; 
    }
    struct shm_cdt *h = (struct shm_cdt*)handle; 

    int result = 0;
    if (unmap_if_mapped(h) == -1) 
        result = -1;
    if (close(h->fd) == -1) 
        result = -1;

    free_shm_handle(h);
    return result;
}



int game_state_map(shm_adt handle, unsigned short width, unsigned short height, game_state_t **out_state) {
    if (!handle || !out_state || width == 0 || height == 0) { 
        errno = EINVAL; 
        return -1; 
    }
    struct shm_cdt *h = (struct shm_cdt*)handle;

    size_t need = game_state_size(width, height);

    if (h->owner && h->size < need) {
        if (ensure_size(h->fd, need) == -1) 
            return -1;
        h->size = need;
    }

    if (!h->base || h->size < need) {
        if (unmap_if_mapped(h) == -1) 
            return -1;
        if (!h->owner && h->size < need) { 
            errno = EINVAL; 
            return -1; 
        } 
        h->base = map_rw(h->fd, h->size);
        if (!h->base) 
            return -1;
    }

    game_state_t *gs = (game_state_t*)h->base;
    *out_state = gs;

    if (h->owner) {
        memset(gs, 0, h->size);
        gs->board_width   = width;
        gs->board_height  = height;
        gs->num_players   = 0;
        gs->game_finished = false;
    }
    return 0;
}


int game_sync_map(shm_adt handle, game_sync_t **out_sync) {
    if (!handle || !out_sync) { 
        errno = EINVAL; 
        return -1; 
    }
    struct shm_cdt *h = (struct shm_cdt*)handle;

    size_t need = sizeof(game_sync_t);

    if (h->owner && h->size < need) {
        if (ensure_size(h->fd, need) == -1) 
            return -1;
        h->size = need;
    }

    if (!h->base || h->size < need) {
        if (unmap_if_mapped(h) == -1) 
            return -1;
        if (!h->owner && h->size < need) { 
            errno = EINVAL; 
            return -1; 
        }
        h->base = map_rw(h->fd, h->size);
        if (!h->base) 
            return -1;
    }

    game_sync_t *sync = (game_sync_t*)h->base;
    *out_sync = sync;

    if (h->owner) {
        if (init_game_sync_semaphores(sync) == -1)
            return -1;
    }

    return 0;
}


int game_state_unmap_destroy(shm_adt handle) {
    if (!handle) { 
        errno = EINVAL; 
        return -1; 
    }
    struct shm_cdt *h = (struct shm_cdt*)handle;
    int result = 0;

    if (unmap_if_mapped(h) == -1) 
        result = -1;
    if (close(h->fd) == -1) 
        result = -1;
    if (h->owner && shm_unlink(h->name) == -1) 
        result = -1;
    free_shm_handle(h);
    return result;
}


int game_sync_unmap_destroy(shm_adt handle) {
    if (!handle) { 
        errno = EINVAL; 
        return -1; 
    }
    struct shm_cdt *h = (struct shm_cdt*)handle;
    int result = 0;
    
    if (h->owner && h->base) {
        game_sync_t *sync = (game_sync_t*)h->base;
        int e = 0;
        e |= sem_destroy(&sync->view_ready);
        e |= sem_destroy(&sync->view_done);
        e |= sem_destroy(&sync->writer_mutex);
        e |= sem_destroy(&sync->state_mutex);
        e |= sem_destroy(&sync->reader_count_mutex);
        for (int i = 0; i < MAX_PLAYERS; ++i) 
            e |= sem_destroy(&sync->player_ready[i]);
        if (e == -1) 
            result = -1;
    }

    if (unmap_if_mapped(h) == -1) 
        result = -1;
    if (close(h->fd) == -1) 
        result = -1;
    if (h->owner && shm_unlink(h->name) == -1) 
        result = -1;

    free_shm_handle(h);
    return result;
}
