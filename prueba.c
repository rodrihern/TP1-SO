#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "shm.h"

int main() {
    shm_adt st = NULL, sy = NULL;
    game_state_t *gs = NULL;
    game_sync_t  *sn = NULL;

    unsigned short W = 5, H = 5;

    printf("=== Creando SHM como master ===\n");

    // Master crea las regiones
    if (shm_region_open(&st, SHM_STATE, game_state_size(W, H)) == -1) {
        perror("shm_region_open state");
        exit(1);
    }
    if (game_state_map(st, W, H, &gs) == -1) {
        perror("game_state_map");
        exit(1);
    }

    if (shm_region_open(&sy, SHM_SYNC, sizeof(game_sync_t)) == -1) {
        perror("shm_region_open sync");
        exit(1);
    }
    if (game_sync_map(sy, &sn) == -1) {
        perror("game_sync_map");
        exit(1);
    }

    // Escribimos datos en game_state
    gs->num_players = 2;
    gs->players[0].score = 10;
    gs->players[1].score = 20;
    for (int y=0; y<H; y++)
        for (int x=0; x<W; x++)
            gs->board[idx(x,y,W)] = (x+y) % 9 + 1;

    printf("Master escribió: players=%u, scores=%u/%u\n",
           gs->num_players,
           gs->players[0].score,
           gs->players[1].score);

    // Simulamos otro proceso que abre las mismas SHM
    printf("\n=== Abriendo SHM como jugador/vista ===\n");

    shm_adt st2 = NULL, sy2 = NULL;
    game_state_t *gs2 = NULL;
    game_sync_t  *sn2 = NULL;

    if (shm_region_open(&st2, SHM_STATE, 1) == -1) {
        perror("shm_region_open state2");
        exit(1);
    }
    if (game_state_map(st2, W, H, &gs2) == -1) {
        perror("game_state_map2");
        exit(1);
    }

    if (shm_region_open(&sy2, SHM_SYNC, sizeof(game_sync_t)) == -1) {
        perror("shm_region_open sync2");
        exit(1);
    }
    if (game_sync_map(sy2, &sn2) == -1) {
        perror("game_sync_map2");
        exit(1);
    }

    // Leemos lo que el "master" escribió
    printf("Jugador lee: players=%u, scores=%u/%u\n",
           gs2->num_players,
           gs2->players[0].score,
           gs2->players[1].score);
    printf("Celda (2,2) = %d\n", gs2->board[idx(2,2,W)]);

    // Limpieza
    game_state_unmap_destroy(st);
    game_sync_unmap_destroy(sy);

    // El jugador/vista solo cierra, no destruye
    shm_region_close(st2);
    shm_region_close(sy2);

    return 0;
}
