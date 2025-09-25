// This is a personal academic project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

#include "common.h"
#include "shm.h"
#include "reader_sync.h"
#include "writer_sync.h"

typedef struct {
    int board_width;
    int board_height;
    int delay_ms;
    int timeout_s;
    unsigned seed;
    const char *view_bin;
    char* player_bins[MAX_PLAYERS];
    int num_players;
} game_args_t;

typedef struct { 
    int read_fd; // read fd (extremo de lectura del pipe)
    int write_fd; // write fd (extremo de escritura del pipe)
    pid_t pid; // pid del proceso hijo
    int alive; // indica si el proceso hijo está vivo
} pipe_info_t;


static void die(const char *m, int error_code) { 
    perror(m); 
    exit(error_code); 
}

static int clamp(int v,int lo,int hi) { 
    return v < lo ? lo : (v > hi ? hi : v); 
}

static void init_board(game_state_t *gs, unsigned seed) {
    srand(seed);
    for(int y=0;y<gs->board_height;y++)
        for(int x=0;x<gs->board_width;x++)
            gs->board[idx(x,y,gs->board_width)] = (rand()%(MAX_REWARD-MIN_REWARD+1))+MIN_REWARD;
}

static void place_players(game_state_t *gs){
    int W=gs->board_width, H=gs->board_height, P=(int)gs->num_players;
    for(int i=0;i<P;i++){
        int x = (i+1)*W/(P+1);
        int y = (i%2? H/3 : (2*H)/3);
        gs->players[i].x = (unsigned short)clamp(x,0,W-1);
        gs->players[i].y = (unsigned short)clamp(y,0,H-1);
        gs->players[i].score=0;
        gs->players[i].valid_moves=0;
        gs->players[i].invalid_moves=0;
        gs->players[i].is_blocked=false;
        snprintf(gs->players[i].name, MAX_NAME_LEN, "P%d", i);
        gs->board[idx(gs->players[i].x, gs->players[i].y, W)] = player_to_cell_value(i);
    }
}

static int apply_move(game_state_t * gs, int pid_idx, unsigned char dir){
    if(!is_valid_direction(dir)){ 
        gs->players[pid_idx].invalid_moves++; 
        return 0; 
    }
    int dx,dy; 
    get_direction_offset((direction_t)dir, &dx, &dy);
    int W=gs->board_width, H=gs->board_height;
    int nx = (int)gs->players[pid_idx].x + dx;
    int ny = (int)gs->players[pid_idx].y + dy;
    if (!is_inside(nx,ny,W,H)) { 
        gs->players[pid_idx].invalid_moves++; 
        return 0; 
    }

    int *cell = &gs->board[idx(nx,ny,W)];
    if (!cell_is_free(*cell)) { 
        gs->players[pid_idx].invalid_moves++; 
        return 0; 
    }

    gs->players[pid_idx].x = (unsigned short)nx;
    gs->players[pid_idx].y = (unsigned short)ny;
    gs->players[pid_idx].score += (unsigned)*cell;
    gs->players[pid_idx].valid_moves++;
    *cell = player_to_cell_value(pid_idx);
    return 1;
}

// static bool has_valid_neighbor_at_locked(const game_state_t *gs, int x, int y){
//     int W = (int)gs->board_width;
//     int H = (int)gs->board_height;
//     for (direction_t d = 0; d < NUM_DIRECTIONS; d++){
//         int dx, dy; 
//         get_direction_offset(d, &dx, &dy);
//         int nx = x + dx, ny = y + dy;
//         if (is_inside(nx, ny, W, H)){
//             if (gs->board[idx(nx, ny, W)] > 0)
//                 return true;
//         }
//     }
//     return false;
// }

static void exec_with_board_args(const char *bin, int board_width, int board_height, const char *error_msg) {
    char wb[16], hb[16];
    snprintf(wb, sizeof wb, "%d", board_width);
    snprintf(hb, sizeof hb, "%d", board_height);
    execl(bin, bin, wb, hb, (char*)NULL);
    perror(error_msg);
    _exit(EXEC_ERROR_CODE);
}

static void finish(game_sync_t * sync, game_state_t * gs, const char * view_bin, pipe_info_t *pipes) {
    printf("DEBUG: Game finishing, killing remaining processes...\n");
    writer_enter(sync);
    gs->game_finished = true;
    writer_exit(sync);
    
    // Terminar todos los procesos hijos que sigan vivos con SIGKILL directamente
    for (unsigned i = 0; i < gs->num_players; ++i) {
        if (pipes[i].alive && pipes[i].pid > 0) {
            printf("Killing player %u (pid=%d) with SIGKILL\n", i, pipes[i].pid);
            kill(pipes[i].pid, SIGKILL);
        }
        sem_post(&sync->player_ready[i]);
    }
    if (view_bin) 
        sem_post(&sync->view_ready);
}

static game_args_t parse_args(int argc, char **argv) {
    game_args_t args;
    args.board_width = MIN_BOARD_SIZE;
    args.board_height = MIN_BOARD_SIZE;
    args.delay_ms = DEFAULT_DELAY_MS;
    args.timeout_s = DEFAULT_TIMEOUT_S;
    args.seed = (unsigned)time(NULL);
    args.view_bin = NULL;
    args.num_players = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && argc > i + 1)
            args.board_width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && argc > i + 1)
            args.board_height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && argc > i + 1)
            args.delay_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && argc > i + 1)
            args.timeout_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && argc > i + 1)
            args.seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v") && argc > i + 1)
            args.view_bin = argv[++i];
        else if (!strcmp(argv[i], "-p")) {
            while (argc > i + 1 && args.num_players < MAX_PLAYERS && argv[i + 1][0] != '-')
                args.player_bins[args.num_players++] = argv[++i];
        }
        else {
            die("Usage: ./master [-w width] [-h height] [-d delay] [-s seed] [-v view] [-t timeout] -p player1 player2...", ERROR_INVALID_ARGS);
        }
    }
    return args;
}

static void validate_game_args(int *board_width, int *board_height, int num_players) {
    *board_width = clamp(*board_width, MIN_BOARD_SIZE, MAX_BOARD_SIZE);
    *board_height = clamp(*board_height, MIN_BOARD_SIZE, MAX_BOARD_SIZE);
    if (num_players < 1) {
        die("Error: At least one player must be specified using -p", ERROR_INVALID_ARGS);
    }
}

static void print_game_args(const game_args_t *args) {
    system("clear");
    printf("width: %d\n", args->board_width);
    printf("height: %d\n", args->board_height);
    printf("delay: %d\n", args->delay_ms);
    printf("timeout: %d\n", args->timeout_s);
    printf("seed: %u\n", args->seed);
    printf("view: %s\n", args->view_bin ? args->view_bin : "(none)");
    printf("num_players: %d\n", args->num_players);
    for (int i = 0; i < args->num_players; i++) {
        printf("  %s\n", args->player_bins[i]);
    }
    struct timespec ts = {.tv_sec = 2};
    nanosleep(&ts, NULL);
    system("clear");
}




int main(int argc, char **argv){
    game_args_t args = parse_args(argc, argv);
    int board_width = args.board_width;
    int board_height = args.board_height;
    int delay_ms = args.delay_ms;
    int timeout_s = args.timeout_s;
    unsigned seed = args.seed;
    const char *view_bin = args.view_bin;
    char **player_bins = args.player_bins;
    int num_players = args.num_players;

    validate_game_args(&board_width, &board_height, num_players);

    print_game_args(&args);
    
    shm_adt game_state_shm, game_sync_shm;
    if (shm_region_open(&game_state_shm, SHM_STATE, game_state_size(board_width,board_height)) == -1) 
        die("Error: failed to open or create shared memory region for game state", ERROR_SHM);
    if (shm_region_open(&game_sync_shm,  SHM_SYNC, sizeof(game_sync_t)) == -1) 
        die("Error: failed to open or create shared memory region for game sync", ERROR_SHM);

    game_state_t *gs=NULL; 
    game_sync_t *sync=NULL;
    if (game_state_map(game_state_shm, (unsigned short)board_width, (unsigned short)board_height, &gs) == -1) 
        die("Error: failed to map game state shared memory", ERROR_SHM);
    if (game_sync_map(game_sync_shm, &sync) == -1) 
        die("Error: failed to map game sync shared memory", ERROR_SHM);

    writer_enter(sync);
    gs->board_width = (unsigned short) board_width;
    gs->board_height = (unsigned short) board_height;
    gs->num_players = (unsigned) num_players;
    gs->game_finished = false;
    init_board(gs, seed);
    place_players(gs);
    writer_exit(sync);

    pid_t view_pid = -1; 
    if (view_bin){ 
        view_pid = fork();
        if (view_pid<0) 
            die("Error: could not fork view process", ERROR_FORK);
        if (view_pid == 0){ 
            exec_with_board_args(view_bin, board_width, board_height, "Error: failed to exec player");
        } 
    }

    pipe_info_t pipes[MAX_PLAYERS] = {0};
    for (int i=0;i<num_players;i++){
        int fds[2]; 
        if (pipe(fds)==-1) 
            die("Error: could not create pipe for player process", ERROR_PIPE);
        pipes[i].read_fd = fds[0]; 
        pipes[i].write_fd = fds[1]; 
        pipes[i].alive=1;
        fcntl(pipes[i].read_fd, F_SETFL, O_NONBLOCK); 

        pid_t pid = fork();
        if (pid < 0) 
            die("Error: could not fork player process", ERROR_FORK);
        if (pid == 0) { // TODO: cerrar todos los pipes de los hijos
            for (int j = 0; j < i; j++) {
                close(pipes[j].read_fd);
            }
            dup2(pipes[i].write_fd, 1); 
            close(pipes[i].read_fd);  
            close(pipes[i].write_fd); 
            exec_with_board_args(player_bins[i], board_width, board_height, "Error: failed to exec player");
        } else { 
            close(pipes[i].write_fd);
            pipes[i].pid = pid;
            writer_enter(sync);
            gs->players[i].pid = pid; 
            const char *bn = player_bins[i];
            const char *slash = strrchr(bn, '/');
            const char *pname = slash ? slash + 1 : bn;
            snprintf(gs->players[i].name, MAX_NAME_LEN, "%s", pname);
            writer_exit(sync);
        }
    }


    

    for (unsigned i=0; i<gs->num_players; i++)
        sem_post(&sync->player_ready[i]);

    if (view_bin){ 
        sem_post(&sync->view_ready); 
        sem_wait(&sync->view_done);
    }

    struct timespec last_valid;  
    clock_gettime(CLOCK_MONOTONIC, &last_valid);
    unsigned next_idx = 0; 

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        time_t elapsed = now.tv_sec - last_valid.tv_sec;
        if (elapsed >= timeout_s) {
            finish(sync, gs, view_bin, pipes);
            break; 
        }
        time_t remain = timeout_s - elapsed;

        bool to_block[MAX_PLAYERS] = {0};
        int fds_to_close[MAX_PLAYERS];
        for (unsigned i=0;i<MAX_PLAYERS;i++) fds_to_close[i] = -1;

        reader_enter(sync);
        for (unsigned i = 0; i < gs->num_players; i++){
            if (gs->players[i].is_blocked) 
                continue;
            int x = (int)gs->players[i].x;
            int y = (int)gs->players[i].y;
        }
        reader_exit(sync);

        bool any_blocked_now = false;
        for (unsigned i = 0; i < gs->num_players; i++){
            if (!to_block[i]) 
                continue;
            writer_enter(sync);
            gs->players[i].is_blocked = true;
            writer_exit(sync);
            if (fds_to_close[i] >= 0){ 
                close(fds_to_close[i]);
                pipes[i].read_fd = -1; 
            }
            any_blocked_now = true;
        }
        if (any_blocked_now && view_bin){
            sem_post(&sync->view_ready);
            sem_wait(&sync->view_done);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        unsigned active_cnt = 0;

 
        bool blocked[MAX_PLAYERS];
        reader_enter(sync);
        for (unsigned i = 0; i < gs->num_players; i++)
            blocked[i] = gs->players[i].is_blocked;
        reader_exit(sync);

        for (unsigned i = 0; i < gs->num_players; i++) {
            if (!blocked[i] && pipes[i].read_fd >= 0) {
                FD_SET(pipes[i].read_fd, &rfds);
                if (pipes[i].read_fd > maxfd) 
                    maxfd = pipes[i].read_fd;
                active_cnt++;
            }
        }

        if (active_cnt == 0) {
            finish(sync, gs, view_bin, pipes);
            break; 
        }

        struct timeval tv;
        tv.tv_sec  = remain;
        tv.tv_usec = 0;

        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) 
                continue; 
            die("Error: select() failed while waiting for player input", ERROR_SELECT);
        }
        if (ready == 0) {
            finish(sync, gs, view_bin, pipes);
            break;
        }

        for (unsigned step = 0; step < gs->num_players; step++) {
            unsigned i = (next_idx + step) % gs->num_players;
            int fd = pipes[i].read_fd;
            if (fd < 0 || blocked[i]) 
                continue;
            if (!FD_ISSET(fd, &rfds)) 
                continue;  

            unsigned char dir;
            ssize_t n = read(fd, &dir, 1);
            if (n == 0) {
                writer_enter(sync);
                gs->players[i].is_blocked = true;
                writer_exit(sync);
                close(fd);
                pipes[i].read_fd = -1;
                pipes[i].alive = 0;
                continue;
            } else if (n < 0) {
                if (errno == EAGAIN) 
                    continue;   
                writer_enter(sync);
                gs->players[i].is_blocked = true;
                writer_exit(sync);
                close(fd);
                pipes[i].read_fd = -1;
                pipes[i].alive = 0;
                continue;
            }

            int was_valid;
            unsigned int invalid_before, invalid_after;
            writer_enter(sync);
            invalid_before = gs->players[i].invalid_moves;
            was_valid = apply_move(gs, (int)i, dir);
            invalid_after = gs->players[i].invalid_moves;
            writer_exit(sync);

            if (was_valid) 
                clock_gettime(CLOCK_MONOTONIC, &last_valid);

            // Actualizar la vista también cuando aumentan los movimientos inválidos
            if ((was_valid || invalid_after != invalid_before) && view_bin) {
                sem_post(&sync->view_ready);
                sem_wait(&sync->view_done);
            }

            if (was_valid && delay_ms > 0) {
                struct timespec ts = { .tv_sec = delay_ms/1000,
                                    .tv_nsec = (delay_ms%1000)*1000000L };
                nanosleep(&ts, NULL);
            }

            sem_post(&sync->player_ready[i]);
        }
        next_idx = (next_idx + 1) % gs->num_players;
    }


    int status;

    if (view_pid>0) {
        waitpid(view_pid,&status,0); 
        if (WIFEXITED(status)) {
            printf("view exited (%d)\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("view signal=%d\n", WTERMSIG(status));
        }
    }    
    
    for (unsigned i=0;i<gs->num_players;i++){
        int code = -1;
        if (pipes[i].pid>0) {
            // SIGKILL y waitpid bloqueante - debería ser inmediato
            // kill(pipes[i].pid, SIGKILL);
            waitpid(pipes[i].pid, &status, 0);
            if (WIFEXITED(status)) code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) code = WTERMSIG(status);
        }
        unsigned score, vld, inv;
        char namebuf[MAX_NAME_LEN];
        reader_enter(sync);
        score = gs->players[i].score;
        vld = gs->players[i].valid_moves;
        inv = gs->players[i].invalid_moves;
        snprintf(namebuf, sizeof namebuf, "%s", gs->players[i].name);
        reader_exit(sync);

        printf("Player %s (%u) exited (%d) with a score of %u / %u / %u\n",
               namebuf, i, code, score, vld, inv);
        if (pipes[i].read_fd >= 0) {
            close(pipes[i].read_fd);
            pipes[i].read_fd = -1;
        }
    }
    
    game_state_unmap_destroy(game_state_shm);
    game_sync_unmap_destroy(game_sync_shm);
    return SUCCESS;
}


