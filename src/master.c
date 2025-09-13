// This is a personal academic project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
//Habilita funcionalidades POSIX (p. ej. clock_gettime, pselect, etc.) según el estándar 2008.
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
    *cell = player_to_cell_value(pid_idx); // capturada por idx (valor negativo)
    return 1;
}

static void exec_with_board_args(const char *bin, int board_width, int board_height, const char *error_msg) {
    char wb[16], hb[16];
    snprintf(wb, sizeof wb, "%d", board_width);
    snprintf(hb, sizeof hb, "%d", board_height);
    execl(bin, bin, wb, hb, (char*)NULL);
    perror(error_msg);
    _exit(EXEC_ERROR_CODE);
}

static void finish(game_sync_t * sync, game_state_t * gs, const char * view_bin) {
    writer_enter(sync);
    gs->game_finished = true;
    writer_exit(sync);
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

static void init_shared_memory(shm_adt *game_state_shm, shm_adt *game_sync_shm, game_state_t **gs, game_sync_t **sync, const game_args_t *args) {
    if (shm_region_open(game_state_shm, SHM_STATE, game_state_size(args->board_width, args->board_height)) == -1)
        die("Error: failed to open or create shared memory region for game state", ERROR_SHM);

    if (shm_region_open(game_sync_shm, SHM_SYNC, sizeof(game_sync_t)) == -1)
        die("Error: failed to open or create shared memory region for game sync", ERROR_SHM);

    if (game_state_map(*game_state_shm, (unsigned short)args->board_width, (unsigned short)args->board_height, gs) == -1)
        die("Error: failed to map game state shared memory", ERROR_SHM);

    if (game_sync_map(*game_sync_shm, sync) == -1)
        die("Error: failed to map game sync shared memory", ERROR_SHM);
}

static void init_game_state(game_sync_t *sync, game_state_t *gs, const game_args_t *args) {
    writer_enter(sync);
    gs->board_width  = (unsigned short) args->board_width;
    gs->board_height = (unsigned short) args->board_height;
    gs->num_players  = (unsigned) args->num_players;
    gs->game_finished = false;
    init_board(gs, args->seed);
    place_players(gs);
    writer_exit(sync);
}



static pid_t spawn_view(const char *view_bin, int board_width, int board_height) {
    if (!view_bin) 
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        die("Error: could not fork view process", ERROR_FORK);
    if (pid == 0) {
        exec_with_board_args(view_bin, board_width, board_height, "Error: failed to exec view");
    }
    return pid;
}

static void spawn_players(pipe_info_t pipes[], game_state_t *gs, game_sync_t *sync, const game_args_t *args) {
    for (int i = 0; i < args->num_players; i++) {
        int fds[2];
        if (pipe(fds) == -1)
            die("Error: could not create pipe for player process", ERROR_PIPE);
        pipes[i].read_fd = fds[0];
        pipes[i].write_fd = fds[1];
        pipes[i].alive = 1;
        fcntl(pipes[i].read_fd, F_SETFL, O_NONBLOCK);

        pid_t pid = fork();
        if (pid < 0)
            die("Error: could not fork player process", ERROR_FORK);
        if (pid == 0) {
            dup2(pipes[i].write_fd, 1);
            close(pipes[i].read_fd);
            exec_with_board_args(args->player_bins[i], args->board_width, args->board_height, "Error: failed to exec player");
        } else {
            close(pipes[i].write_fd);
            pipes[i].pid = pid;
            writer_enter(sync);
            gs->players[i].pid = pid;
            const char *bn = args->player_bins[i];
            const char *slash = strrchr(bn, '/');
            const char *pname = slash ? slash + 1 : bn;
            snprintf(gs->players[i].name, MAX_NAME_LEN, "%s", pname);
            writer_exit(sync);
        }
    }
}

static void enable_first_moves(game_sync_t *sync, unsigned num_players) {
    for (unsigned i = 0; i < num_players; i++)
        sem_post(&sync->player_ready[i]);
}

static void notify_view_start(game_sync_t *sync, const char *view_bin) {
    if (view_bin) {
        sem_post(&sync->view_ready); // Notifica a la vista que hay cambios
        sem_wait(&sync->view_done);  // Espera a que la vista termine de imprimir
    }
}


// Prepara los file descriptors activos y devuelve la cantidad de jugadores activos
static unsigned prepare_active_fds(pipe_info_t pipes[], bool blocked[], unsigned num_players, fd_set *rfds, int *maxfd) {
    FD_ZERO(rfds); // Inicializa el conjunto de file descriptors para que esté vacío
    unsigned active_cnt = 0;
    *maxfd = -1;
    for (unsigned i = 0; i < num_players; i++) {
        if (!blocked[i] && pipes[i].read_fd >= 0) {
            FD_SET(pipes[i].read_fd, rfds);
            if (pipes[i].read_fd > *maxfd)
                *maxfd = pipes[i].read_fd;
            active_cnt++;
        }
    }
    return active_cnt;
}

// Procesa el movimiento de un jugador
static void process_player_move(game_state_t *gs, game_sync_t *sync, pipe_info_t *pipe, unsigned i, struct timespec *last_valid, const char *view_bin, int delay_ms) {
    int fd = pipe->read_fd;
    unsigned char dir;
    ssize_t n = read(fd, &dir, 1);
    if (n == 0 || (n < 0 && errno != EAGAIN)) {
        writer_enter(sync);
        gs->players[i].is_blocked = true;
        writer_exit(sync);
        close(fd);
        pipe->read_fd = -1;
        pipe->alive = 0;
        return;
    }

    int was_valid;
    writer_enter(sync);
    was_valid = apply_move(gs, (int)i, dir);
    writer_exit(sync);

    if (was_valid) 
        clock_gettime(CLOCK_MONOTONIC, last_valid);

    if (view_bin) {
        sem_post(&sync->view_ready);
        sem_wait(&sync->view_done);
    }

    if (delay_ms > 0) {
        struct timespec ts = { .tv_sec = delay_ms/1000,
                               .tv_nsec = (delay_ms%1000)*1000000L };
        nanosleep(&ts, NULL);
    }

    sem_post(&sync->player_ready[i]);
}

static bool check_game_timeout(struct timespec *last_valid, int timeout_s, game_sync_t *sync, game_state_t *gs, const char *view_bin) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    time_t elapsed = now.tv_sec - last_valid->tv_sec;
    if (elapsed >= timeout_s) {
        finish(sync, gs, view_bin);
        return true;
    }
    return false;
}

static bool timeout_expired(const struct timespec *last_valid, int timeout_s) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - last_valid->tv_sec) >= timeout_s;
}

static time_t remaining_time(const struct timespec *last_valid, int timeout_s) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return timeout_s - (now.tv_sec - last_valid->tv_sec);
}

static void snapshot_blocked(game_state_t *gs, game_sync_t *sync, bool blocked[]) {
    reader_enter(sync);
    for (unsigned i = 0; i < gs->num_players; i++)
        blocked[i] = gs->players[i].is_blocked;
    reader_exit(sync);
}

static unsigned build_fdset(pipe_info_t pipes[], bool blocked[], unsigned nplayers, fd_set *rfds, int *maxfd) {
    return prepare_active_fds(pipes, blocked, nplayers, rfds, maxfd);
}

static int wait_for_activity(fd_set *rfds, int maxfd, time_t remain) {
    struct timeval tv = { .tv_sec = remain, .tv_usec = 0 };
    int ready = select(maxfd + 1, rfds, NULL, NULL, &tv);
    if (ready < 0 && errno != EINTR) {
        die("Error: select() failed while waiting for player input", ERROR_SELECT);
    }
    return ready;
}

static void process_ready_players(game_state_t *gs, game_sync_t *sync,pipe_info_t pipes[], fd_set *rfds, unsigned nplayers, unsigned *next_idx, struct timespec *last_valid, const char *view_bin, int delay_ms) {
    for (unsigned step = 0; step < nplayers; step++) {
        unsigned i = (*next_idx + step) % nplayers;
        int fd = pipes[i].read_fd;
        if (fd < 0 || gs->players[i].is_blocked) 
            continue;
        if (!FD_ISSET(fd, rfds)) 
            continue;

        process_player_move(gs, sync, &pipes[i], i, last_valid, view_bin, delay_ms);
    }
    *next_idx = (*next_idx + 1) % nplayers;
}


static void game_loop(game_state_t *gs, game_sync_t *sync, pipe_info_t pipes[], const char *view_bin, int delay_ms, int timeout_s) {
    struct timespec last_valid;
    clock_gettime(CLOCK_MONOTONIC, &last_valid);
    unsigned next_idx = 0;

    while (1) {
        if (timeout_expired(&last_valid, timeout_s)) {
            finish(sync, gs, view_bin);
            break;
        }
        time_t remain = remaining_time(&last_valid, timeout_s);

        bool blocked[MAX_PLAYERS];
        snapshot_blocked(gs, sync, blocked);

        fd_set rfds;
        int maxfd;
        unsigned active_cnt = build_fdset(pipes, blocked, gs->num_players, &rfds, &maxfd);

        if (active_cnt == 0) {
            finish(sync, gs, view_bin);
            break;
        }

        int ready = wait_for_activity(&rfds, maxfd, remain);
        if (ready < 0) continue;
        if (ready == 0) {
            finish(sync, gs, view_bin);
            break;
        }

        process_ready_players(gs, sync, pipes, &rfds, gs->num_players, &next_idx, &last_valid, view_bin, delay_ms);
    }
}


static void wait_and_report_view(pid_t view_pid) {
    int status;
    if (view_pid > 0) {
        waitpid(view_pid, &status, 0);
        if (WIFEXITED(status)) {
            printf("view exit=%d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("view signal=%d\n", WTERMSIG(status));
        }
    }
}

static void wait_and_report_players(game_state_t *gs, game_sync_t *sync, pipe_info_t pipes[]) {
    int status;
    for (unsigned i = 0; i < gs->num_players; i++) {
        int code = -1;
        if (pipes[i].pid > 0) {
            waitpid(pipes[i].pid, &status, 0);
            if (WIFEXITED(status)) 
                code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) 
                code = WTERMSIG(status);
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
        close(pipes[i].read_fd);
    }
}

static void cleanup_resources(shm_adt game_state_shm, shm_adt game_sync_shm) {
    game_state_unmap_destroy(game_state_shm);
    game_sync_unmap_destroy(game_sync_shm);
}

/* ----------------------------- main ------------------------------- */


int main(int argc, char **argv){
    game_args_t args = parse_args(argc, argv);
    validate_game_args(&args.board_width, &args.board_height, args.num_players);

    shm_adt game_state_shm, game_sync_shm;
    game_state_t *gs = NULL;
    game_sync_t *sync = NULL;
    init_shared_memory(&game_state_shm, &game_sync_shm, &gs, &sync, &args);

    init_game_state(sync, gs, &args);

    pipe_info_t pipes[MAX_PLAYERS] = {0};
    spawn_players(pipes, gs, sync, &args);

    pid_t view_pid = spawn_view(args.view_bin, args.board_width, args.board_height);

    enable_first_moves(sync, gs->num_players);
    notify_view_start(sync, args.view_bin);

    game_loop(gs, sync, pipes, args.view_bin, args.delay_ms, args.timeout_s);

    wait_and_report_view(view_pid);
    wait_and_report_players(gs, sync, pipes);
    cleanup_resources(game_state_shm, game_sync_shm);
    return SUCCESS;
}


