#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "common.h"
#include "shm.h"
#include "reader_sync.h"


static void print_state(game_state_t *gs){
    /* limpiar pantalla */
    printf("\033[H\033[J");
    printf("ChompChamps %ux%u  players=%u  finished=%d\n",
           gs->board_width, gs->board_height, gs->num_players, gs->game_finished);
    for (unsigned i=0;i<gs->num_players;i++){
        player_t *p = &gs->players[i];
        printf("P%u(pid=%d)%s  pos=(%u,%u)  score=%u  V=%u  I=%u\n",
               i,(int)p->pid, p->is_blocked?" [BLOCKED]":"",
               p->x,p->y,p->score,p->valid_moves,p->invalid_moves);
    }
    /* recorte de tablero para no inundar */
    unsigned W=gs->board_width, H=gs->board_height;
    unsigned w=W>40?40:W, h=H>20?20:H;
    puts("Board:");
    for (unsigned y=0;y<h;y++){
        for (unsigned x=0;x<w;x++){
            int v = gs->board[idx(x,y,W)];
            if (v<=0) 
                printf("%2d ", v); 
            else 
                printf(" %d ", v);
        }
        putchar('\n');
    }
    fflush(stdout);
}

int main(int argc, char **argv){
    if (argc<3){ 
        fprintf(stderr,"uso: vista <W> <H>\n"); 
        return ERROR_INVALID_ARGS; 
    }
    int W = atoi(argv[1]), H = atoi(argv[2]);

    shm_adt state_h, sync_h;
    if (shm_region_open(&state_h, SHM_STATE, game_state_size(W,H)) == -1) { 
        perror("state open"); 
        return ERROR_SHM_ATTACH; 
    }
    if (shm_region_open(&sync_h, SHM_SYNC, sizeof(game_sync_t)) == -1) { 
        perror("sync open");  
        return ERROR_SHM_ATTACH; 
    }

    game_state_t *gs=NULL; game_sync_t *sync=NULL;
    if (game_state_map(state_h, (unsigned short)W, (unsigned short)H, &gs) == -1) { 
        perror("map state"); 
        return ERROR_SHM_ATTACH; 
    }
    if (game_sync_map(sync_h, &sync) == -1) { 
        perror("map sync"); 
        return ERROR_SHM_ATTACH; 
    }

    while (1){
        sem_wait(&sync->view_ready);
        reader_enter(sync);
        int finished = gs->game_finished;
        print_state(gs);
        reader_exit(sync);
        sem_post(&sync->view_done);
        if (finished) 
            break;
    }

    game_state_unmap_destroy(state_h);
    game_sync_unmap_destroy(sync_h);
    return SUCCESS;
}
