// This is a personal academic project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
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
#define C_PLAYER_BASE 2

static void ui_init(void){
    if (getenv("TERM") == NULL) { 
        setenv("TERM", "xterm-256color", 1); // para que corra con el master de la catedra
    }
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
        // Colores por jugador (fg,bg) según pedido: 
        init_pair(C_PLAYER_BASE + 0, COLOR_RED, -1);
        init_pair(C_PLAYER_BASE + 1, COLOR_GREEN, -1);
        init_pair(C_PLAYER_BASE + 2, COLOR_BLUE, -1);
        init_pair(C_PLAYER_BASE + 3, COLOR_MAGENTA, -1);
        init_pair(C_PLAYER_BASE + 4, COLOR_YELLOW, -1);
        init_pair(C_PLAYER_BASE + 5, COLOR_CYAN, -1);
        init_pair(C_PLAYER_BASE + 6, COLOR_BLACK, -1);
        init_pair(C_PLAYER_BASE + 7, COLOR_YELLOW, COLOR_BLUE);
        init_pair(C_PLAYER_BASE + 8, COLOR_RED, COLOR_WHITE);
    }
}

static void ui_end(void){
    nodelay(stdscr, FALSE);
    endwin();
}

// Ya no coloreamos las recompensas: se imprimen en color por defecto

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
        mvprintw(row++, 0, "P%u %s  pos=(%u,%u)  score=%u  V=%u  I=%u",
                 i, p->is_blocked?" [BLOCKED]":"",
                 p->x, p->y, p->score, p->valid_moves, p->invalid_moves);
        attroff(COLOR_PAIR(pc));
    }
    return row; // próxima fila libre
}

static void draw_board_centered(const game_state_t *gs, int reserve_top_rows){
    int maxy, maxx; 
    getmaxyx(stdscr, maxy, maxx);
    const int cellw = 4; // ancho por celda: suficiente para "p[8]" o "%3d"

    int bw = gs->board_width, bh = gs->board_height;
    int draw_h = bh;
    int draw_w = bw;
    // Limitar por tamaño de terminal
    if (draw_h > maxy - 2) 
        draw_h = maxy - 2; // deja margen
    if (draw_w * cellw > maxx - 2) 
        draw_w = (maxx - 2) / cellw;
    if (draw_h <= 0 || draw_w <= 0) 
        return;

    // Centro ideal
    int row0 = (maxy - draw_h) / 2;
    int col0 = (maxx - draw_w * cellw) / 2;
    // Evitar superponerse con header/lista de jugadores
    if (row0 <= reserve_top_rows) row0 = reserve_top_rows + 1;
    if (col0 < 0) col0 = 0;

    // mvprintw(row0 - 1, col0, "Board (%dx%d shown of %dx%d):", draw_w, draw_h, bw, bh);

    for (int y = 0; y < draw_h; ++y){
        int sy = row0 + y; 
        if (sy >= maxy) 
            break;
        for (int x = 0; x < draw_w; ++x){
            int sx = col0 + x * cellw; 
            if (sx + (cellw-1) >= maxx) 
                break;

            // ¿Hay un jugador parado en (x,y)?
            int standing_pid = -1;
            for (unsigned i = 0; i < gs->num_players; ++i){
                const player_t *p = &gs->players[i];
                if ((int)p->x == x && (int)p->y == y){ 
                    standing_pid = (int)i; 
                    break;
                }
            }

            if (standing_pid >= 0){
                short pc = color_for_player((unsigned)standing_pid);
                attron(COLOR_PAIR(pc) | A_BOLD);
                // p[id] con ancho 4 (ej: p[8] )
                mvprintw(sy, sx, "p[%d]", standing_pid);
                attroff(COLOR_PAIR(pc) | A_BOLD);
                continue;
            }

            int v = gs->board[idx(x, y, bw)];
            if (v > 0){
                // Recompensas sin color especial
                attron(COLOR_PAIR(C_DEFAULT));
                mvprintw(sy, sx, "%3d ", v);
                attroff(COLOR_PAIR(C_DEFAULT));
            } else {
                // 0 o negativo: se imprime el número tal cual, coloreado por jugador
                unsigned pid = (unsigned)(-v);
                short pc = color_for_player(pid);
                attron(COLOR_PAIR(pc) | A_BOLD);
                mvprintw(sy, sx, "%3d ", v);
                attroff(COLOR_PAIR(pc) | A_BOLD);
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
    draw_board_centered(gs, next_row + 1);
        refresh();

        reader_exit(sync);
        
        if (finished) {
            int maxy, maxx;
            getmaxyx(stdscr, maxy, maxx);
            const char *msg = "Juego Terminado - presiona q para salir";
            mvprintw(maxy - 1, 0, "%s", msg);
            refresh();

            nodelay(stdscr, FALSE);
            int ch;
            do {
                ch = getch();
            } while (ch != 'q' && ch != 'Q');
        }
        
        sem_post(&sync->view_done);
        if (finished)
            break;

        int a;
        
    }

    ui_end();

    game_state_unmap_destroy(state_h);
    game_sync_unmap_destroy(sync_h);
    return SUCCESS;
}
