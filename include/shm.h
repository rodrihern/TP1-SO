#ifndef SHM_H
#define SHM_H

#pragma once
#include <stddef.h>
#include <semaphore.h>
#include "common.h"

typedef struct shm_cdt * shm_adt;

// Crea/abre una región de memoria compartida (SHM) y devuelve un “handle” vía *out_handle. Retorna 0 si salió bien, -1 si hubo error (y deja errno seteado).
int shm_region_open(shm_adt* out_handle, const char* name, size_t size_bytes);
// Desmapea si hacía falta, cierra el fd y libera el handle.
int shm_region_close(shm_adt handle);

// Mapea el game state, calcula el tamaño requerido incluyendo el array flexible board[].
int game_state_map(shm_adt handle, unsigned short width, unsigned short height, game_state_t** out_state);
// Mapea la memoria para los semaforos
int game_sync_map (shm_adt handle, game_sync_t** out_sync);

int game_state_unmap_destroy(shm_adt handle);
int game_sync_unmap_destroy (shm_adt handle);

#endif