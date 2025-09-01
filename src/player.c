// jugador.c
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "common.h"
#include "shm.h"
#include "reader_sync.h"

/* heurística simple: primera dirección con celda libre alrededor */
static int pick_dir(game_state_t *game_state, int me){
    int current_x = game_state->players[me].x, current_y = game_state->players[me].y;
    for (unsigned char d=0; d<NUM_DIRECTIONS; ++d){
        int dx,dy; 
        get_direction_offset((direction_t)d, &dx, &dy);
        int nx=current_x+dx, ny=current_y+dy;
        if (!is_inside(nx,ny,game_state->board_width,game_state->board_height)) 
            continue;
        if (cell_is_free(game_state->board[idx(nx,ny,game_state->board_width)])) 
            return d;
    }
    return -1;
}

int main(int argc, char **argv){
    if (argc<3){ 
        fprintf(stderr,"uso: jugador <W> <H>\n"); 
        return ERROR_INVALID_ARGS; 
    }
    int width = atoi(argv[1]), height = atoi(argv[2]);

    shm_adt state_h, sync_h;
    if (shm_region_open(&state_h, SHM_STATE, game_state_size(width,height)) == -1) { 
        perror("state open"); 
        return ERROR_SHM_ATTACH; 
    }
    if (shm_region_open(&sync_h,  SHM_SYNC, sizeof(game_sync_t)) == -1) { 
        perror("sync open");  
        return ERROR_SHM_ATTACH; 
    }

    game_state_t *game_state=NULL; 
    game_sync_t *sync=NULL;
    if (game_state_map(state_h, (unsigned short)width, (unsigned short)height, &game_state) == -1) { 
        perror("map state"); 
        return ERROR_SHM_ATTACH; 
    }
    if (game_sync_map(sync_h, &sync) == -1) { 
        perror("map sync"); 
        return ERROR_SHM_ATTACH; 
    }

    /* encontrar mi índice por PID */
    pid_t me = getpid();
    int me_idx = -1;
    reader_enter(sync);
    for (unsigned i=0;i<game_state->num_players;i++){
        if (game_state->players[i].pid == me){ 
            me_idx = (int)i; 
            break;
        }
    }
    reader_exit(sync);
    if (me_idx<0){ 
        fprintf(stderr,"jugador: no encuentro mi PID en game_state\n"); 
        return 2; 
    }

    /* bucle principal */
    while (1){
        /* esperar a que el master procese lo anterior */
        sem_wait(&sync->player_ready[me_idx]);

        /* check fin de juego */
        reader_enter(sync);
        int finished = game_state->game_finished;
        reader_exit(sync);
        if (finished) break;

        /* decidir movimiento */
        reader_enter(sync);
        int dir = pick_dir(game_state, me_idx);
        reader_exit(sync);

        if (dir < 0){
            /* sin movimientos -> EOF al master */
            fflush(stdout);
            close(1);
            break;
        } else {
            unsigned char b = (unsigned char)dir;
            if (write(1, &b, 1) < 0) 
                break;
        }
        /* el master hará sem_post(player_ready[me_idx]) cuando procese */
    }

    game_state_unmap_destroy(state_h);
    game_sync_unmap_destroy(sync_h);
    return SUCCESS;
}
