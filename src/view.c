// Vista ncurses a color con tablero fijo
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ncurses.h>

#include "common.h"
#include "shm.h"
#include "reader_sync.h"

// Pares de color
#define C_DEFAULT 1
#define C_REWARD1 2
#define C_REWARD2 3
#define C_REWARD3 4
#define C_REWARD4 5
#define C_REWARD5 6
#define C_REWARD6 7
#define C_REWARD7 8
#define C_REWARD8 9
#define C_REWARD9 10
#define C_PLAYER_BASE 20

static void ui_init(void){
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE); // no bloquear en getch
    keypad(stdscr, TRUE);
    if (has_colors()){
        start_color();
        use_default_colors();
        init_pair(C_DEFAULT, COLOR_WHITE, -1);
        init_pair(C_REWARD1, COLOR_BLUE, -1);
        init_pair(C_REWARD2, COLOR_CYAN, -1);
        init_pair(C_REWARD3, COLOR_GREEN, -1);
        init_pair(C_REWARD4, COLOR_YELLOW, -1);
        init_pair(C_REWARD5, COLOR_MAGENTA, -1);
        init_pair(C_REWARD6, COLOR_RED, -1);
        init_pair(C_REWARD7, COLOR_WHITE, -1);
        init_pair(C_REWARD8, COLOR_CYAN, -1);
        init_pair(C_REWARD9, COLOR_YELLOW, -1);
        // Colores para jugadores (9 distintas, se cicla si hay más)
        init_pair(C_PLAYER_BASE + 0, COLOR_RED, -1);
        init_pair(C_PLAYER_BASE + 1, COLOR_GREEN, -1);
        init_pair(C_PLAYER_BASE + 2, COLOR_YELLOW, -1);
        init_pair(C_PLAYER_BASE + 3, COLOR_BLUE, -1);
        init_pair(C_PLAYER_BASE + 4, COLOR_MAGENTA, -1);
        init_pair(C_PLAYER_BASE + 5, COLOR_CYAN, -1);
        init_pair(C_PLAYER_BASE + 6, COLOR_WHITE, -1);
        init_pair(C_PLAYER_BASE + 7, COLOR_BLUE, -1);
        init_pair(C_PLAYER_BASE + 8, COLOR_RED, -1);
    }
}

static void ui_end(void){
    nodelay(stdscr, FALSE);
    endwin();
}

static short color_for_reward(int v){
    switch(v){
        case 1: return C_REWARD1; case 2: return C_REWARD2; case 3: return C_REWARD3;
        case 4: return C_REWARD4; case 5: return C_REWARD5; case 6: return C_REWARD6;
        case 7: return C_REWARD7; case 8: return C_REWARD8; case 9: return C_REWARD9;
        default: return C_DEFAULT;
    }
}

static short color_for_player(unsigned id){
    return C_PLAYER_BASE + (id % 9);
}

static void draw_header(const game_state_t *gs){
    attron(A_BOLD);
    mvprintw(0, 0, "ChompChamps %ux%u  players=%u  finished=%d",
             gs->board_width, gs->board_height, gs->num_players, gs->game_finished);
    attroff(A_BOLD);
}

static int draw_players(const game_state_t *gs, int start_row){
    mvprintw(start_row, 0, "Players:");
    int row = start_row + 1;
    for (unsigned i = 0; i < gs->num_players; ++i){
        const player_t *p = &gs->players[i];
        short pc = color_for_player(i);
        attron(COLOR_PAIR(pc));
        mvprintw(row++, 0, "P%u(pid=%d)%s  pos=(%u,%u)  score=%u  V=%u  I=%u",
                 i, (int)p->pid, p->is_blocked?" [BLOCKED]":"",
                 p->x, p->y, p->score, p->valid_moves, p->invalid_moves);
        attroff(COLOR_PAIR(pc));
    }
    return row; // próxima fila libre
}

static void draw_board(const game_state_t *gs, int start_row, int start_col){
    int maxy, maxx; getmaxyx(stdscr, maxy, maxx);
    const int cellw = 3; // " n "
    int avail_rows = maxy - start_row - 1;
    int avail_cols = maxx - start_col;
    if (avail_rows <= 0 || avail_cols <= 0) return;

    int bw = gs->board_width, bh = gs->board_height;
    int draw_h = avail_rows; if (draw_h > bh) draw_h = bh;
    int draw_w = avail_cols / cellw; if (draw_w > bw) draw_w = bw;

    mvprintw(start_row, start_col, "Board (%dx%d shown of %dx%d):", draw_w, draw_h, bw, bh);
    int row0 = start_row + 1;

    for (int y = 0; y < draw_h; ++y){
        int sy = row0 + y; if (sy >= maxy) break;
        for (int x = 0; x < draw_w; ++x){
            int sx = start_col + x * cellw; if (sx + (cellw-1) >= maxx) break;
            int v = gs->board[idx(x, y, bw)];
            if (v > 0){
                short c = color_for_reward(v);
                attron(COLOR_PAIR(c));
                mvprintw(sy, sx, " %d ", v);
                attroff(COLOR_PAIR(c));
            } else if (v < 0){
                int owner = -v; // dueño 1..N
                if (owner >= 1){
                    short pc = color_for_player((unsigned)(owner-1));
                    attron(COLOR_PAIR(pc) | A_BOLD);
                    // Pxx (2 dígitos)
                    mvprintw(sy, sx, "P%02d", owner);
                    attroff(COLOR_PAIR(pc) | A_BOLD);
                } else {
                    attron(COLOR_PAIR(C_DEFAULT));
                    mvprintw(sy, sx, " · ");
                    attroff(COLOR_PAIR(C_DEFAULT));
                }
            } else { // v == 0
                attron(COLOR_PAIR(C_DEFAULT));
                mvprintw(sy, sx, " · ");
                attroff(COLOR_PAIR(C_DEFAULT));
            }
        }
    }
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

    ui_init();
    while (1){
        sem_wait(&sync->view_ready);
        reader_enter(sync);
        int finished = gs->game_finished;

        erase();
        draw_header(gs);
        int next_row = draw_players(gs, 2);
        draw_board(gs, next_row + 1, 0);
        mvprintw(LINES-1, 0, "q para salir");
        refresh();

        reader_exit(sync);
        sem_post(&sync->view_done);

        int ch = getch();
        if (ch == 'q' || ch == 'Q') break;
        if (finished) break;
    }

    ui_end();

    game_state_unmap_destroy(state_h);
    game_sync_unmap_destroy(sync_h);
    return SUCCESS;
}
