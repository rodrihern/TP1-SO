// master.c
//Habilita funcionalidades POSIX (p. ej. clock_gettime, pselect, etc.) según el estándar 2008.
#define _POSIX_C_SOURCE 200809L
//Incluye librerías para IPC, procesos, timers, SHM y multiplexación de E/S.
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

#include "common.h"
#include "shm.h"

/* ---------- helpers de sincronización (lectores/escritor) ---------- */
static inline void reader_enter(game_sync_t *s) {
    sem_wait(&s->writer_mutex); 
    sem_wait(&s->reader_count_mutex); 
    if (++s->reader_count == 1) sem_wait(&s->state_mutex);
    sem_post(&s->reader_count_mutex); //NO SIGNIFICA YA TERMINE DE LEER, SIGNIFICA YA TERMINE MI ENTRADA
    sem_post(&s->writer_mutex); //EVITA QUE LOS LECTORES SE SIGAN METIENDO SI UN ESCRITOR ESTA ESPERANDO
}

static inline void reader_exit(game_sync_t *s) {
    sem_wait(&s->reader_count_mutex);
    if (--s->reader_count == 0) sem_post(&s->state_mutex); 
    sem_post(&s->reader_count_mutex);
}
static inline void writer_enter(game_sync_t *s) {
    sem_wait(&s->writer_mutex);
    sem_wait(&s->state_mutex);
}
static inline void writer_exit(game_sync_t *s) {
    sem_post(&s->state_mutex);
    sem_post(&s->writer_mutex);
}

/* ---------------------------- utilidades --------------------------- */
static void die(const char *m) { perror(m); exit(1); }
static int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }

static void init_board(game_state_t *gs, unsigned seed){
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
        gs->players[i].x = (unsigned short)clampi(x,0,W-1);
        gs->players[i].y = (unsigned short)clampi(y,0,H-1);
        gs->players[i].score=0;
        gs->players[i].valid_moves=0;
        gs->players[i].invalid_moves=0;
        gs->players[i].is_blocked=false;
        snprintf(gs->players[i].name, MAX_NAME_LEN, "P%d", i);
        // Las celdas iniciales NO otorgan recompensa ni se marcan capturadas
    }
}

static int apply_move(game_state_t *gs, int pid_idx, unsigned char dir){
    if(!is_valid_direction(dir)){ gs->players[pid_idx].invalid_moves++; return 0; }
    int dx,dy; get_direction_offset((direction_t)dir, &dx, &dy);
    int W=gs->board_width, H=gs->board_height;
    int nx = (int)gs->players[pid_idx].x + dx;
    int ny = (int)gs->players[pid_idx].y + dy;
    if (!is_inside(nx,ny,W,H)) { gs->players[pid_idx].invalid_moves++; return 0; }

    int *cell = &gs->board[idx(nx,ny,W)];
    if (!cell_is_free(*cell)) { gs->players[pid_idx].invalid_moves++; return 0; }

    gs->players[pid_idx].x = (unsigned short)nx;
    gs->players[pid_idx].y = (unsigned short)ny;
    gs->players[pid_idx].score += (unsigned)*cell;
    gs->players[pid_idx].valid_moves++;
    *cell = player_to_cell_value(pid_idx); // capturada por idx (valor negativo)
    return 1;
}

/* ----------------------------- main ------------------------------- */
typedef struct { int rfd; int wfd; pid_t pid; int alive; } pipe_info_t;

int main(int argc, char **argv){
    /* defaults */
    int W=MIN_BOARD_SIZE, H=MIN_BOARD_SIZE, delay_ms=200, timeout_s=10;
    unsigned seed=(unsigned)time(NULL);
    const char *view_bin=NULL;
    char* player_bins[MAX_PLAYERS]; int P=0;

    /* parseo simple */
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"-w") && i+1<argc) W=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-h") && i+1<argc) H=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-d") && i+1<argc) delay_ms=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-t") && i+1<argc) timeout_s=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-s") && i+1<argc) seed=(unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i],"-v") && i+1<argc) view_bin=argv[++i];
        else if (!strcmp(argv[i],"-p")){
            while (i+1<argc && P<MAX_PLAYERS && argv[i+1][0]!='-') player_bins[P++]=argv[++i];
        } else { fprintf(stderr,"arg desconocido: %s\n", argv[i]); return ERROR_INVALID_ARGS; }
    }
    W = clampi(W, MIN_BOARD_SIZE, MAX_BOARD_SIZE);
    H = clampi(H, MIN_BOARD_SIZE, MAX_BOARD_SIZE);
    if (P<1){ fprintf(stderr,"Se requiere al menos 1 jugador con -p\n"); return ERROR_INVALID_ARGS; }

    /* SHM: abrir/crear con tu API */
    shm_adt state_h, sync_h;
    if (shm_region_open(&state_h, SHM_STATE, game_state_size(W,H)) == -1) die("shm_region_open state");
    if (shm_region_open(&sync_h,  SHM_SYNC,  sizeof(game_sync_t))       == -1) die("shm_region_open sync");

    game_state_t *gs=NULL; game_sync_t *sync=NULL;
    if (game_state_map(state_h, (unsigned short)W, (unsigned short)H, &gs) == -1) die("game_state_map");
    if (game_sync_map(sync_h, &sync) == -1) die("game_sync_map");

    /* inicialización de estado */
    writer_enter(sync);
    gs->board_width  = (unsigned short)W;
    gs->board_height = (unsigned short)H;
    gs->num_players  = (unsigned)P;
    gs->game_finished= false;
    init_board(gs, seed);
    place_players(gs);
    writer_exit(sync);

    /* pipes & fork jugadores */
    pipe_info_t pipes[MAX_PLAYERS] = {0};
    for (int i=0;i<P;i++){
        int fds[2]; if (pipe(fds)==-1) die("pipe");
        pipes[i].rfd = fds[0]; pipes[i].wfd = fds[1]; pipes[i].alive=1;
        fcntl(pipes[i].rfd, F_SETFL, O_NONBLOCK);

        pid_t pid = fork();
        if (pid<0) die("fork player");
        if (pid==0){
            /* hijo jugador: dup write-end -> fd=1 */
            dup2(pipes[i].wfd, 1);
            close(pipes[i].rfd);
            /* argv: width height */
            char wb[16], hb[16];
            snprintf(wb,sizeof wb,"%d",W);
            snprintf(hb,sizeof hb,"%d",H);
            execl(player_bins[i], player_bins[i], wb, hb, (char*)NULL);
            perror("exec player"); _exit(127);
        } else {
            close(pipes[i].wfd); // master no escribe
            pipes[i].pid = pid;
            writer_enter(sync);
            gs->players[i].pid = pid;
            writer_exit(sync);
        }
    }

    /* fork vista (opcional) */
    pid_t view_pid = -1;
    if (view_bin){
        pid_t vp = fork();
        if (vp<0) die("fork view");
        if (vp==0){
            char wb[16], hb[16];
            snprintf(wb,sizeof wb,"%d",W);
            snprintf(hb,sizeof hb,"%d",H);
            execl(view_bin, view_bin, wb, hb, (char*)NULL);
            perror("exec view"); _exit(127);
        } else view_pid = vp;
    }

    /* habilitar primer movimiento */
    for (unsigned i=0;i<gs->num_players;i++) sem_post(&sync->player_ready[i]);

    /* primer print si hay vista */
    if (view_bin){ sem_post(&sync->view_ready); sem_wait(&sync->view_done); }

    /* main loop */
    struct timespec last_valid; clock_gettime(CLOCK_MONOTONIC, &last_valid);
    unsigned next_idx = 0;

    while (1){
        /* timeout global */
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - last_valid.tv_sec) > timeout_s) {
            writer_enter(sync); gs->game_finished = true; writer_exit(sync);
            if (view_bin){ sem_post(&sync->view_ready); }
            break;
        }

        int progressed = 0;
        for (unsigned step=0; step<gs->num_players; ++step){
            unsigned i = (next_idx + step) % gs->num_players;

            /* ya bloqueado? */
            reader_enter(sync);
            int blocked = gs->players[i].is_blocked;
            reader_exit(sync);
            if (blocked) continue;

            /* ¿hay byte en pipe? */
            fd_set rfds; FD_ZERO(&rfds); FD_SET(pipes[i].rfd, &rfds);
            struct timeval tv = {0,0};
            int r = select(pipes[i].rfd+1, &rfds, NULL, NULL, &tv);
            if (r<=0 || !FD_ISSET(pipes[i].rfd, &rfds)) continue;

            unsigned char dir;
            ssize_t n = read(pipes[i].rfd, &dir, 1);
            if (n==0){
                /* EOF: jugador queda bloqueado */
                writer_enter(sync);
                gs->players[i].is_blocked = true;
                writer_exit(sync);
                pipes[i].alive = 0;
                continue;
            }
            if (n<0){
                if (errno==EAGAIN) continue;
                continue;
            }

            /* aplicar UNA solicitud */
            int was_valid;
            writer_enter(sync);
            was_valid = apply_move(gs, (int)i, dir);
            writer_exit(sync);

            if (was_valid) clock_gettime(CLOCK_MONOTONIC, &last_valid);

            /* notificar vista */
            if (view_bin){ sem_post(&sync->view_ready); sem_wait(&sync->view_done); }

            /* delay */
            struct timespec ts={ .tv_sec=delay_ms/1000, .tv_nsec=(delay_ms%1000)*1000000L };
            nanosleep(&ts,NULL);

            /* habilitar próximo movimiento del mismo jugador */
            sem_post(&sync->player_ready[i]);

            progressed = 1;
        }
        next_idx = (next_idx + 1) % gs->num_players;

        /* ¿terminó porque todos están bloqueados? */
        int any_unblocked = 0;
        reader_enter(sync);
        for (unsigned i=0;i<gs->num_players;i++){
            if (!gs->players[i].is_blocked){ any_unblocked=1; break; }
        }
        reader_exit(sync);
        if (!any_unblocked){
            writer_enter(sync); gs->game_finished = true; writer_exit(sync);
            if (view_bin){ sem_post(&sync->view_ready); }
            break;
        }

        if (!progressed){
            struct timespec tiny={0,5*1000000L}; nanosleep(&tiny,NULL); // 5ms
        }
    }

    /* esperar hijos y reportar puntajes */
    int status;
    for (unsigned i=0;i<gs->num_players;i++){
        if (pipes[i].pid>0) waitpid(pipes[i].pid,&status,0);
        reader_enter(sync);
        printf("Player %u (pid=%d): score=%u V=%u I=%u%s\n",
            i,(int)gs->players[i].pid, gs->players[i].score,
            gs->players[i].valid_moves, gs->players[i].invalid_moves,
            gs->players[i].is_blocked? " [BLOCKED]":"");
        reader_exit(sync);
        close(pipes[i].rfd);
    }
    if (view_pid>0) waitpid(view_pid,&status,0);

    /* limpiar SHM (el unlink lo hace solo el owner según tu shm.c) */
    game_state_unmap_destroy(state_h);
    game_sync_unmap_destroy(sync_h);
    return SUCCESS;
}
