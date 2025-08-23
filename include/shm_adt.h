#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "common.h"  // acá definís Player, GameState y GameSync

/**
 * TAD para manejo de memoria compartida en ChompChamps
 * Se encarga de crear, mapear, desmapear y destruir
 * las dos regiones requeridas: /game_state y /game_sync.
 */

// --- Estructura genérica que guarda info de la SHM ---
typedef shm_cdt* shm_adt;

// --- Funciones genéricas ---
/**
 * Crea o se conecta a una SHM.
 * - Si no existe: la crea con 'size' y el caller es owner.
 * - Si existe: se conecta y el caller NO es owner.
 */
int shm_region_open(shm_adt* r, const char* name, size_t size);

/**
 * Desmapea y cierra el fd.
 */
int shm_region_close(shm_adt r);

/**
 * Elimina (shm_unlink) sólo si el proceso era owner.
 */
int shm_region_unlink_if_owner(shm_adt r);

// --- Funciones específicas del juego ---

/**
 * Mapea la memoria compartida de estado (/game_state).
 * Devuelve puntero tipado a GameState en *out.
 * Si owner, inicializa dimensiones, jugadores y tablero.
 */
int game_state_map(shm_adt r,
                   unsigned short width,
                   unsigned short height,
                   game_state_t** out);

/**
 * Mapea la memoria compartida de sincronización (/game_sync).
 * Devuelve puntero tipado a GameSync en *out.
 * Si owner, inicializa semáforos y contador F.
 */
int game_sync_map(shm_adt r, game_sync_t** out);

/**
 * Helpers para limpieza al final del juego.
 */
int game_state_unmap_destroy(shm_adt r);
int game_sync_unmap_destroy(shm_adt r);

