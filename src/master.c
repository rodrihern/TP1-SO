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
#include "reader_sync.h"
#include "writer_sync.h"


static void die(const char *m) { 
    perror(m); 
    exit(1); 
}

/**
 * Limita el valor 'v' para que esté dentro del rango [lo, hi].
 * Si 'v' es menor que 'lo', retorna 'lo'.
 * Si 'v' es mayor que 'hi', retorna 'hi'.
 * Si 'v' está en el rango, retorna 'v'.
 */
static int clamp(int v,int lo,int hi){ 
    return v<lo?lo:(v>hi?hi:v); 
}

// Usa la semilla para inicializar el tablero con números aleatorios
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
        gs->players[i].x = (unsigned short)clamp(x,0,W-1);
        gs->players[i].y = (unsigned short)clamp(y,0,H-1);
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

/* ----------------------------- main ------------------------------- */
typedef struct { 
    int read_fd; // read fd (extremo de lectura del pipe)
    int write_fd; // write fd (extremo de escritura del pipe)
    pid_t pid; // pid del proceso hijo
    int alive; // indica si el proceso hijo está vivo
} pipe_info_t;

int main(int argc, char **argv){
    // Inicializo con valores por defecto
    int board_width=MIN_BOARD_SIZE, board_height=MIN_BOARD_SIZE, delay_ms=DEFAULT_DELAY_MS, timeout_s=DEFAULT_TIMEOUT_S;
    unsigned seed=(unsigned)time(NULL); 
    const char *view_bin=NULL;
    char* player_bins[MAX_PLAYERS]; 
    int num_players=0;

    // Parseo de los argumentos de la línea de comandos 
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"-w") && argc>i+1) 
            board_width=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-h") && argc>i+1) 
            board_height=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-d") && argc>i+1) 
            delay_ms=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-t") && argc>i+1) 
            timeout_s=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-s") && argc>i+1) 
            seed=(unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i],"-v") && argc>i+1) 
            view_bin=argv[++i];
        else if (!strcmp(argv[i],"-p")){
            while (argc>i+1 && num_players<MAX_PLAYERS && argv[i+1][0]!='-') 
                player_bins[num_players++]=argv[++i];
        } 
        else { 
            fprintf(stderr,"arg desconocido: %s\n", argv[i]); 
            return ERROR_INVALID_ARGS; 
        }
    }

    // Validaciones del tamaño del tablero
    board_width = clamp(board_width, MIN_BOARD_SIZE, MAX_BOARD_SIZE);
    board_height = clamp(board_height, MIN_BOARD_SIZE, MAX_BOARD_SIZE);
    if (num_players<1){ 
        fprintf(stderr,"Se requiere al menos 1 jugador con -p\n"); 
        return ERROR_INVALID_ARGS; 
    }

    // SHM: abrir/crear con tu API
    shm_adt game_state_shm, game_syng_shm;
    if (shm_region_open(&game_state_shm, SHM_STATE, game_state_size(board_width,board_height)) == -1) 
        die("shm_region_open state");
    if (shm_region_open(&game_syng_shm,  SHM_SYNC, sizeof(game_sync_t)) == -1) 
        die("shm_region_open sync");

    game_state_t *gs=NULL; game_sync_t *sync=NULL;
    if (game_state_map(game_state_shm, (unsigned short)board_width, (unsigned short)board_height, &gs) == -1) 
        die("game_state_map");
    if (game_sync_map(game_syng_shm, &sync) == -1) 
        die("game_sync_map");

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
            die("pipe");
        pipes[i].read_fd = fds[0]; 
        pipes[i].write_fd = fds[1]; 
        pipes[i].alive=1;
        fcntl(pipes[i].read_fd, F_SETFL, O_NONBLOCK); // Habilitar modo no bloqueante en el extremo de lectura. 
        // Así, las operaciones de lectura (read) sobre ese pipe no se detendrán esperando datos; si no hay datos, retornan inmediatamente con error EAGAIN
        // Así el master puede seguir funcionando aunque un jugador no envíe datos

        pid_t pid = fork();
        if (pid < 0) // Hubo un error
            die("fork player");
        if (pid == 0){ // Estoy en el proceso hijo
            // hijo jugador: dup write-end -> fd=1 
            dup2(pipes[i].write_fd, 1); // Redirige el extremo de escritura del pipe al stdout (fd=1). Así, todo lo que el jugador escriba por printf va al pipe.
            close(pipes[i].read_fd);  // Cierra el extremo de lectura del pipe (el hijo no lee el pipe)
            // argv: width height 
            exec_with_board_args(player_bins[i], board_width, board_height, "exec player");
        } else { // Estoy en el proceso padre
            close(pipes[i].write_fd); // master no escribe
            pipes[i].pid = pid;
            writer_enter(sync);
            gs->players[i].pid = pid; //fork te devuelve el PID del hijo si estas en el padre
            writer_exit(sync);
        }
    }

    // fork vista 
    pid_t view_pid = -1; // PID del proceso de la vista (si se crea)
    if (view_bin){ 
        view_pid = fork();
        if (view_pid<0) 
            die("fork view");
        if (view_pid == 0){ // estamos en el hijo: la vista
            exec_with_board_args(view_bin, board_width, board_height, "exec view");
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


    // game loop
    while (1){
        // Verificar si ha pasado el tiempo de espera
        struct timespec now; 
        clock_gettime(CLOCK_MONOTONIC, &now); // Obtener el tiempo actual
        if ((now.tv_sec - last_valid.tv_sec) > timeout_s) {
            writer_enter(sync);  // Entra como escritor a para marcar que terminó
            gs->game_finished = true; 
            writer_exit(sync);
            if (view_bin){ 
                sem_post(&sync->view_ready); 
            }
            break; // Salir del game loop
        }

        //Verificar si algún jugador ha avanzado
        //Va a intentar atender a cada jugador a lo sumo una vez en esta vuelta (round‑robin).
        //next_idx recuerda dónde quedó la ronda anterior (evita sesgo).
        int progressed = 0; // Flag para indicar si algún jugador avanzó
        for (unsigned step = 0; step < gs->num_players; step++){
            unsigned i = (next_idx + step) % gs->num_players; // recorre los jugadores arrancando desde next_idx

            // ya bloqueado? 
            //Lectura protegida como lector (jugadores y vista también leen).
            //Si ya está bloqueado (sin más movimientos posibles / EOF), lo salteás.
            reader_enter(sync);
            int blocked = gs->players[i].is_blocked;
            reader_exit(sync); // Entro como lector
            if (blocked) 
                continue; // Anda al prox jugador

            // ¿hay byte en pipe? 
            //Usa select con timeout 0 (no bloquea): chequea si el pipe del jugador i tiene un byte listo.
            //Si no hay nada, sigue con el siguiente jugador.
            fd_set read_fd_set; 
            FD_ZERO(&read_fd_set); 
            FD_SET(pipes[i].read_fd, &read_fd_set);
            struct timeval tv = {0,0};
            int r = select(pipes[i].read_fd+1, &read_fd_set, NULL, NULL, &tv);
            if (r<=0 || !FD_ISSET(pipes[i].read_fd, &read_fd_set)) 
                continue; // Anda al prox jugador


            // procesa el movimiento
            unsigned char dir;
            ssize_t n = read(pipes[i].read_fd, &dir, 1);
            if (n==0){
                // EOF: jugador queda bloqueado 
                writer_enter(sync);
                gs->players[i].is_blocked = true;
                writer_exit(sync);
                pipes[i].alive = 0;
                continue;
            } else if (n<0){
                if (errno==EAGAIN) 
                    continue;
                continue;
            }

            // aplicar UNA solicitud 
            int was_valid;
            writer_enter(sync);
            was_valid = apply_move(gs, (int)i, dir);
            writer_exit(sync);

            if (was_valid) 
                clock_gettime(CLOCK_MONOTONIC, &last_valid);

            // notificar vista (se hace luego de procesar CADA solicitud) 
            if (view_bin){ 
                sem_post(&sync->view_ready); // A++ (le avisa a la vista que hay cambios por imprimir)
                sem_wait(&sync->view_done); // B-- (espera a que la vista termine de imprimir)
            }

            // delay 
            struct timespec ts={ 
                .tv_sec=delay_ms/1000,
                .tv_nsec=(delay_ms%1000)*1000000L 
            };
            nanosleep(&ts,NULL);

            // habilitar próximo movimiento del mismo jugador 
            sem_post(&sync->player_ready[i]);

            progressed = 1;
        }
        next_idx = (next_idx + 1) % gs->num_players;

        // ¿terminó porque todos están bloqueados? 
        int any_unblocked = 0;
        reader_enter(sync);
        for (unsigned i=0;i<gs->num_players;i++){
            if (!gs->players[i].is_blocked){ 
                any_unblocked=1; 
                break; 
            }
        }
        reader_exit(sync);
        if (!any_unblocked){
            writer_enter(sync); 
            gs->game_finished = true; writer_exit(sync);
            if (view_bin){ 
                sem_post(&sync->view_ready); 
            }
            break;
        }

        if (!progressed){
            struct timespec tiny={0,5*1000000L}; 
            nanosleep(&tiny,NULL); // 5ms
        }
    }

    int status;

    if (view_pid>0) // Si existe el proceso vista...
        waitpid(view_pid,&status,0); // ...espera a que termine el proceso vista

    // esperar hijos y reportar puntajes 
    for (unsigned i=0;i<gs->num_players;i++){
        if (pipes[i].pid>0) 
            waitpid(pipes[i].pid,&status,0); // Espera a que termine el proceso del jugador
        reader_enter(sync);
        printf("Player %u (pid=%d): score=%u V=%u I=%u%s\n",
            i,(int)gs->players[i].pid, gs->players[i].score,
            gs->players[i].valid_moves, gs->players[i].invalid_moves,
            gs->players[i].is_blocked? " [BLOCKED]":"");
        reader_exit(sync);
        close(pipes[i].read_fd);
    }
    

    // limpiar SHM (el unlink lo hace solo el owner según tu shm.c) 
    game_state_unmap_destroy(game_state_shm);
    game_sync_unmap_destroy(game_syng_shm);
    return SUCCESS;
}
