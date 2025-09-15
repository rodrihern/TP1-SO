// This is a personal academic project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

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
#include "player.h"

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

int find_player_index(game_state_t *game_state, game_sync_t *sync, pid_t me) {
    int idx = -1;
    reader_enter(sync);
    for (unsigned i = 0; i < game_state->num_players; i++) {
        if (game_state->players[i].pid == me) {
            idx = (int)i;
            break;
        }
    }
    reader_exit(sync);
    return idx;
}

int init_shared_memory(int width, int height, shm_adt *state_h, shm_adt *sync_h, game_state_t **game_state, game_sync_t **sync) {
    if (shm_region_open(state_h, SHM_STATE, game_state_size(width, height)) == -1)
        return ERROR_SHM_ATTACH;
    if (shm_region_open(sync_h, SHM_SYNC, sizeof(game_sync_t)) == -1)
        return ERROR_SHM_ATTACH;
    if (game_state_map(*state_h, (unsigned short)width, (unsigned short)height, game_state) == -1)
        return ERROR_SHM_ATTACH;
    if (game_sync_map(*sync_h, sync) == -1)
        return ERROR_SHM_ATTACH;
    return SUCCESS;
}

int is_game_finished(game_state_t *game_state, game_sync_t *sync) {
    reader_enter(sync);
    int finished = game_state->game_finished;
    reader_exit(sync);
    return finished;
}

void get_player_position(game_state_t *game_state, game_sync_t *sync, int my_idx, int *x, int *y) {
    reader_enter(sync);
    *x = game_state->players[my_idx].x;
    *y = game_state->players[my_idx].y;
    reader_exit(sync);
}


int main(int argc, char * argv[]){
    if (argc<NUM_ARGS){ 
        fprintf(stderr,"uso: jugador <W> <H>\n"); 
        return ERROR_INVALID_ARGS; 
    }
    int width = atoi(argv[1]), height = atoi(argv[2]);

    shm_adt state_h, sync_h;
    game_state_t *game_state = NULL;
    game_sync_t *sync = NULL;
    if (init_shared_memory(width, height, &state_h, &sync_h, &game_state, &sync) != SUCCESS) {
        perror("Error: failed to initialize shared memory");
        return ERROR_SHM_ATTACH;
    }

    pid_t me = getpid();
    int my_idx = find_player_index(game_state, sync, me);
    if (my_idx<0){ 
        fprintf(stderr,"jugador: no encuentro mi PID en game_state\n"); 
        return 2; 
    }

    int * board_copy = malloc(width * height * sizeof(*board_copy));
    if (!board_copy) {
    perror("Error: failed to allocate memory for board_copy");
        game_state_unmap_destroy(state_h);
        game_sync_unmap_destroy(sync_h);
        return ERROR_SHM_ATTACH;
    }

    while (1) {
        sem_wait(&sync->player_ready[my_idx]);
        if (is_game_finished(game_state, sync))
            break;

        reader_enter(sync);
        int x = game_state->players[my_idx].x;
        int y = game_state->players[my_idx].y;
        memcpy(board_copy, game_state->board, width * height * sizeof(*board_copy));
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

