#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <sys/types.h>
#include <semaphore.h>

#define NUM_DIRECTIONS 8

// Nombres de las memorias compartidas
#define SHM_STATE "/game_state"
#define SHM_SYNC "/game_sync"

// Valores por defecto de tiempo
#define DEFAULT_DELAY_MS 200
#define DEFAULT_TIMEOUT_S 100

// Dimensiones y límites del juego
#define MAX_PLAYERS 9
#define MAX_NAME_LEN 16
#define MIN_BOARD_SIZE 10
#define MAX_BOARD_SIZE 100

// Valores del tablero
#define MIN_REWARD 1
#define MAX_REWARD 9
#define EMPTY_CELL 0

// Códigos de retorno
#define SUCCESS 0                     // No error
#define ERROR_INVALID_ARGS -1         // Invalid command line arguments
#define ERROR_SHM -2                  // Failed to open or create shared memory region for game state
#define ERROR_PIPE -3                 // Failed to create pipe for player process
#define ERROR_FORK -4                 // Failed to fork child process
#define ERROR_SELECT -5               // select() system call failed
#define ERROR_SHM_ATTACH -6           // Failed to attach shared memory
#define EXEC_ERROR_CODE 127           // Exec failure (standard)


typedef enum {
    UP = 0,        
    UP_RIGHT = 1,  
    RIGHT = 2,     
    DOWN_RIGHT = 3,
    DOWN = 4,      
    DOWN_LEFT = 5, 
    LEFT = 6,      
    LEFT_UP = 7    
} direction_t;


typedef struct {
    char name[MAX_NAME_LEN];     // Nombre del jugador
    unsigned int score;          // Puntaje acumulado
    unsigned int invalid_moves;  // Cantidad de movimientos inválidos
    unsigned int valid_moves;    // Cantidad de movimientos válidos
    unsigned short x;            // Coordenada X en el tablero
    unsigned short y;            // Coordenada Y en el tablero
    pid_t pid;                   // Identificador de proceso
    bool is_blocked;             // Indica si el jugador está bloqueado
} player_t;


typedef struct {
    unsigned short board_width;          // Ancho del tablero
    unsigned short board_height;         // Alto del tablero
    unsigned int num_players;            // Cantidad de jugadores
    player_t players[MAX_PLAYERS];       // Lista de jugadores
    bool game_finished;                  // Indica si el juego terminó
    int board[];                         // Tablero (flexible)
} game_state_t;

typedef struct {
    sem_t view_ready;                // A: Master → Vista (hay cambios por imprimir)
    sem_t view_done;                 // B: Vista → Master (terminó de imprimir)
    sem_t writer_mutex;              // C: Mutex para evitar inanición del master al acceder al estado.
    sem_t state_mutex;               // D: Mutex para el estado del juego
    sem_t reader_count_mutex;        // E: Mutex para la variable reader_count (para protegerla)
    unsigned int reader_count;       // F: Cantidad de jugadores leyendo el estado en el instánte actual
    sem_t player_ready[MAX_PLAYERS]; // G: Indica a cada jugador que puede enviar 1 movimiento. Garantiza 1 movimiento por vez por jugador
    // 
} game_sync_t;


static inline int idx(int x, int y, int width) {
    return y * width + x;
}


static inline bool is_inside(int x, int y, int width, int height) {
    return (x >= 0 && x < width && y >= 0 && y < height);
}


static inline bool cell_is_free(int value) {
    return (value >= MIN_REWARD && value <= MAX_REWARD);
}


static inline bool cell_is_captured(int value) {
    return (value <= 0);
}


static inline int get_cell_owner(int value) {
    return cell_is_captured(value) ? (-value) : -1;
}


static inline int player_to_cell_value(int player_id) {
    return -player_id;
}

static inline void get_direction_offset(direction_t dir, int *dx, int *dy) {
    static const int offsets[8][2] = {
        {0, -1},  // UP
        {1, -1},  // UP_RIGHT
        {1, 0},   // RIGHT
        {1, 1},   // DOWN_RIGHT
        {0, 1},   // DOWN
        {-1, 1},  // DOWN_LEFT
        {-1, 0},  // LEFT
        {-1, -1}  // LEFT_UP
    };
    
    if (dir >= 0 && dir <= 7) {
        *dx = offsets[dir][0];
        *dy = offsets[dir][1];
    } else {
        *dx = 0;
        *dy = 0;
    }
}


static inline bool is_valid_direction(unsigned char dir) {
    return (dir <= 7);
}


static inline size_t game_state_size(int width, int height) {
    return sizeof(game_state_t) + (width * height * sizeof(int));
}

#endif 