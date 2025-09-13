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
// Wrapper function for argument parsing
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

/* ----------------------------- main ------------------------------- */
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

    // Validaciones del tamaño del tablero
    board_width = clamp(board_width, MIN_BOARD_SIZE, MAX_BOARD_SIZE);
    board_height = clamp(board_height, MIN_BOARD_SIZE, MAX_BOARD_SIZE);
    if (num_players<1){ 
        die("Error: At least one player must be specified using -p\n", ERROR_INVALID_ARGS);
    }

    // SHM: abrir/crear
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

    // inicialización de estado 
    writer_enter(sync);
    gs->board_width = (unsigned short) board_width;
    gs->board_height = (unsigned short) board_height;
    gs->num_players = (unsigned) num_players;
    gs->game_finished = false;
    init_board(gs, seed);
    place_players(gs);
    writer_exit(sync);

    // pipes & fork jugadores 
    pipe_info_t pipes[MAX_PLAYERS] = {0};
    for (int i=0;i<num_players;i++){
        int fds[2]; 
        if (pipe(fds)==-1) 
            die("Error: could not create pipe for player process", ERROR_PIPE);
        pipes[i].read_fd = fds[0]; 
        pipes[i].write_fd = fds[1]; 
        pipes[i].alive=1;
        fcntl(pipes[i].read_fd, F_SETFL, O_NONBLOCK); // Habilitar modo no bloqueante en el extremo de lectura. 
        // Así, las operaciones de lectura (read) sobre ese pipe no se detendrán esperando datos; si no hay datos, retornan inmediatamente con error EAGAIN
        // Así el master puede seguir funcionando aunque un jugador no envíe datos

        pid_t pid = fork();
        if (pid < 0) // Hubo un error
            die("Error: could not fork player process", ERROR_FORK);
        if (pid == 0){ // Estoy en el proceso hijo
            // hijo jugador: dup write-end -> fd=1 
            dup2(pipes[i].write_fd, 1); // Redirige el extremo de escritura del pipe al stdout (fd=1). Así, todo lo que el jugador escriba por printf va al pipe.
            close(pipes[i].read_fd);  // Cierra el extremo de lectura del pipe (el hijo no lee el pipe)
            // argv: width height 
            exec_with_board_args(player_bins[i], board_width, board_height, "Error: failed to exec player");
        } else { // Estoy en el proceso padre
            close(pipes[i].write_fd); // master no escribe
            pipes[i].pid = pid;
            writer_enter(sync);
            gs->players[i].pid = pid; //fork te devuelve el PID del hijo si estas en el padre
            // Guardar un nombre amigable del jugador a partir del binario (basename)
            const char *bn = player_bins[i];
            const char *slash = strrchr(bn, '/');
            const char *pname = slash ? slash + 1 : bn;
            snprintf(gs->players[i].name, MAX_NAME_LEN, "%s", pname);
            writer_exit(sync);
        }
    }

    // fork vista 
    pid_t view_pid = -1; // PID del proceso de la vista (si se crea)
    if (view_bin){ 
        view_pid = fork();
        if (view_pid<0) 
            die("Error: could not fork view process", ERROR_FORK);
        if (view_pid == 0){ // estamos en el hijo: la vista
            exec_with_board_args(view_bin, board_width, board_height, "Error: failed to exec player");
        } 
    }

    // habilitar primer movimiento de todos los jugadores
    for (unsigned i=0; i<gs->num_players; i++)
        sem_post(&sync->player_ready[i]);

    //primer print al iniciar si hay vista 
    if (view_bin){ 
        sem_post(&sync->view_ready); // A++ (le avisa a la vista que hay cambios por imprimir)
        sem_wait(&sync->view_done); // B-- (espera a que la vista termine de imprimir)
    }

    // main loop 
    struct timespec last_valid;  // timespec esta en <time.h>
    clock_gettime(CLOCK_MONOTONIC, &last_valid);
    unsigned next_idx = 0; // próximo jugador a atender (round-robin sin sesgo)

    while (1) {
        // 1) Timeout global: armá timeout relativo para select
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        time_t elapsed = now.tv_sec - last_valid.tv_sec;
        if (elapsed >= timeout_s) {
            finish(sync, gs, view_bin);
            break; // salgo del bucle principal del juego
        }
        time_t remain = timeout_s - elapsed;

        // 2) Armar fd_set con todos los jugadores NO bloqueados
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        unsigned active_cnt = 0;

        // Tomo snapshot de bloqueados bajo lock de lector.
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

        // Si no queda nadie activo, terminá
        if (active_cnt == 0) {
            finish(sync, gs, view_bin);
            break; // TODO: modularizar esto de que termino el juego
        }

        // 3) Dormir hasta que alguien escriba o venza el timeout global
        struct timeval tv;
        tv.tv_sec  = remain;
        tv.tv_usec = 0;

        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue; // reintentar si fue por señal
            die("Error: select() failed while waiting for player input", ERROR_SELECT);
        }
        if (ready == 0) {
            // venció timeout_s sin movimientos
            finish(sync, gs, view_bin);
            break;
        }

        // 4) Hay al menos un fd listo: procesar en round-robin (1 mov por jugador listo)
        // int progressed = 0;
        for (unsigned step = 0; step < gs->num_players; step++) {
            unsigned i = (next_idx + step) % gs->num_players;
            int fd = pipes[i].read_fd;
            if (fd < 0 || blocked[i]) continue;
            if (!FD_ISSET(fd, &rfds)) continue;  // no estaba listo en esta ronda

            unsigned char dir;
            ssize_t n = read(fd, &dir, 1);
            if (n == 0) {
                // EOF: jugador bloqueado y saco el fd del sistema
                writer_enter(sync);
                gs->players[i].is_blocked = true;
                writer_exit(sync);
                close(fd);
                pipes[i].read_fd = -1;
                pipes[i].alive = 0;
                continue;
            } else if (n < 0) {
                if (errno == EAGAIN) continue;   // raro, pero posible: otro hilo drenó
                // error duro: tratemos como EOF
                writer_enter(sync);
                gs->players[i].is_blocked = true;
                writer_exit(sync);
                close(fd);
                pipes[i].read_fd = -1;
                pipes[i].alive = 0;
                continue;
            }

            // Aplicar exactamente UN movimiento
            int was_valid;
            writer_enter(sync);
            was_valid = apply_move(gs, (int)i, dir);
            writer_exit(sync);

            if (was_valid) clock_gettime(CLOCK_MONOTONIC, &last_valid);

            // Notificar vista por cada movimiento
            if (view_bin) {
                sem_post(&sync->view_ready);
                sem_wait(&sync->view_done);
            }

            // Delay por jugada
            if (delay_ms > 0) {
                struct timespec ts = { .tv_sec = delay_ms/1000,
                                    .tv_nsec = (delay_ms%1000)*1000000L };
                nanosleep(&ts, NULL);
            }

            // Habilitar próximo movimiento del mismo jugador
            sem_post(&sync->player_ready[i]);
        }

        // Avanzar el puntero de fairness
        next_idx = (next_idx + 1) % gs->num_players;

        // Acá NO hace falta un sleep “tiny”; select ya bloquea si nadie escribe
    }


    int status;

    if (view_pid>0) {// Si existe el proceso vista...
        waitpid(view_pid,&status,0); // ...espera a que termine el proceso vista
        if (WIFEXITED(status)) {
            printf("view exit=%d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("view signal=%d\n", WTERMSIG(status));
        }
    }    
    
    // esperar hijos y reportar puntajes (formato requerido)
    for (unsigned i=0;i<gs->num_players;i++){
        int code = -1;
        if (pipes[i].pid>0) {
            waitpid(pipes[i].pid,&status,0); // Espera a que termine el proceso del jugador
            if (WIFEXITED(status)) code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) code = WTERMSIG(status); // mostramos la señal como código
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
    

    // limpiar SHM  
    game_state_unmap_destroy(game_state_shm);
    game_sync_unmap_destroy(game_sync_shm);
    return SUCCESS;
}


