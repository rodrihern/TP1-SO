#pragma once
#include <stddef.h>
#include <semaphore.h>
#include "common.h"

typedef struct shm_cdt * shm_adt;
int shm_region_open(shm_adt* r, const char* name, size_t size);
int shm_region_close(shm_adt r);
int shm_region_unlink_if_owner(shm_adt r);

int game_state_map(shm_adt r, unsigned short w, unsigned short h, game_state_t** out);
int game_sync_map (shm_adt r, game_sync_t** out);

int game_state_unmap_destroy(shm_adt r);
int game_sync_unmap_destroy (shm_adt r);
