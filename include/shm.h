#ifndef SHM_H
#define SHM_H

#pragma once
#include <stddef.h>
#include <semaphore.h>
#include "common.h"

typedef struct shm_cdt * shm_adt;

int shm_region_open(shm_adt* out_handle, const char* name, size_t size_bytes);
int shm_region_close(shm_adt handle);

int game_state_map(shm_adt handle, unsigned short width, unsigned short height, game_state_t** out_state);
int game_sync_map (shm_adt handle, game_sync_t** out_sync);

int game_state_unmap_destroy(shm_adt handle);
int game_sync_unmap_destroy (shm_adt handle);

#endif