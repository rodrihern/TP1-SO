// This is a personal academic project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
// jugador.c
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include "common.h"
#include "shm.h"
#include "reader_sync.h"


int pick_dir(int board[], int width, int height, int x, int y);



int main(int argc, char * argv[]){
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
    int my_idx = -1;
    reader_enter(sync);
    for (unsigned i=0;i<game_state->num_players;i++){
        if (game_state->players[i].pid == me){ 
            my_idx = (int)i; 
            break;
        }
    }
    reader_exit(sync);
    if (my_idx<0){ 
        fprintf(stderr,"jugador: no encuentro mi PID en game_state\n"); 
        return 2; 
    }

    int * board_copy = malloc(width * height * sizeof(*board_copy));

    while (1) {
        // esperar a que pueda jugar
        sem_wait(&sync->player_ready[my_idx]);

        // checkear si termino el juego
        reader_enter(sync);
        int finished = game_state->game_finished;
        reader_exit(sync);
        if (finished)
            break;

        reader_enter(sync);
        int x = game_state->players[my_idx].x;
        int y = game_state->players[my_idx].y;
        board_copy = memcpy(board_copy, game_state->board, width * height * sizeof(*board_copy));
        reader_exit(sync);

        int dir = pick_dir(board_copy, width, height, x, y);

        if (dir < 0) {
            fflush(stdout);
            close(STDOUT_FILENO);
            break;
        } else {
            unsigned char b = (unsigned char)dir;
            if (write(STDOUT_FILENO, &b, 1) < 0) 
                break;
        }
        
    }

    game_state_unmap_destroy(state_h);
    game_sync_unmap_destroy(sync_h);
    free(board_copy);
    return SUCCESS;
}

int pick_dir(int board[], int width, int height, int x, int y) {
    int max_score = 0;
    int dir = -1;

    for (direction_t d = 0; d < NUM_DIRECTIONS; d++) {
        int dx, dy;
        get_direction_offset(d, &dx, &dy);
        if (is_inside(x+dx, y+dy, width, height)) {
            int current_score = board[idx(x+dx, y+dy, width)];
            if (current_score > max_score) {
                max_score = current_score;
                dir = d;
            }
        }
    }

    return dir;
}