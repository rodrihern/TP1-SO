#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <sys/types.h>
#include <semaphore.h>

/*
 * =============================================================================
 * CONSTANTES Y CONFIGURACIONES GLOBALES
 * =============================================================================
 */

#define NUM_DIRECTIONS 8

// Nombres de las memorias compartidas
#define SHM_STATE "/game_state"
#define SHM_SYNC "/game_sync"

// Valores por defecto de tiempo
#define DEFAULT_DELAY_MS 200
#define DEFAULT_TIMEOUT_S 10

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
#define SUCCESS 0
#define ERROR_INVALID_ARGS -1
#define ERROR_SHM_CREATE -2
#define ERROR_SHM_ATTACH -3
#define ERROR_PROCESS_CREATE -4
#define ERROR_TIMEOUT -5
#define ERROR_GAME_OVER -6
#define EXEC_ERROR_CODE 127

/*
 * =============================================================================
 * ENUMERACIONES
 * =============================================================================
 */

/**
 * Direcciones de movimiento (0-7)
 * Comenzando por UP (0) y avanzando en sentido horario
 */
typedef enum {
    UP = 0,        //   (0, -1)
    UP_RIGHT = 1,  //   (1, -1)
    RIGHT = 2,     //   (1,  0)
    DOWN_RIGHT = 3,//   (1,  1)
    DOWN = 4,      //   (0,  1)
    DOWN_LEFT = 5, //   (-1, 1)
    LEFT = 6,      //   (-1, 0)
    LEFT_UP = 7    //   (-1,-1)
} direction_t;

/*
 * =============================================================================
 * ESTRUCTURAS DE DATOS
 * =============================================================================
 */

/**
 * Información de un jugador
 * Anteriormente "XXX" en el enunciado
 */
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

/**
 * Estado completo del juego
 * Anteriormente "YYY" en el enunciado
 * 
 * IMPORTANTE: board[] es un flexible array member que debe ser
 * accedido usando board[y * width + x] para la posición (x,y)
 */
typedef struct {
    unsigned short board_width;          // Ancho del tablero
    unsigned short board_height;         // Alto del tablero
    unsigned int num_players;            // Cantidad de jugadores
    player_t players[MAX_PLAYERS];       // Lista de jugadores
    bool game_finished;                  // Indica si el juego terminó
    int board[];                         // Tablero (flexible)
} game_state_t;

/**
 * Estructura de sincronización entre procesos
 * Anteriormente "ZZZ" en el enunciado
 */
typedef struct {
    sem_t view_ready;              // A: Master → Vista (hay cambios por imprimir)
    sem_t view_done;               // B: Vista → Master (terminó de imprimir)
    sem_t writer_mutex;            // C: Mutex para evitar inanición del master al acceder al estado. 
    // Si un escritor quiere entrar, lo pone en 0 para que no entren más lectores.
    // Arranca en 1 (abierto)
    // Vale 0:
    // - durante la entrada de un lector (muy breve)
    // - durante la escritura hasta writer_exit
    sem_t state_mutex;             // D: Mutex para el estado del juego
    // Lo toma el primer lector y lo suelta el último  -> los escritores lo toman en exclusiva para escribir
    // Vale 0:
    // - cuando hay al menos 1 lector dentro
    // - cuando hay 1 escritor activo (master escribiendo)
    // Vale 1: cuando no hay ni lectores ni escritor
    sem_t reader_count_mutex;      // E: Mutex para la variable reader_count (para protegerla)
    // Vale 0:
    // - mientras un lector este actualizando reader_count
    unsigned int reader_count;     // F: Cantidad de jugadores leyendo el estado en el instánte actual
    // El escritor entra cuando es 0 y D = 1
    sem_t player_ready[MAX_PLAYERS]; // G: Indica a cada jugador que puede enviar 1 movimiento. Garantiza 1 movimiento por vez por jugador
    // 
} game_sync_t;

/*
 * =============================================================================
 * FUNCIONES HELPER INLINE
 * =============================================================================
 */

/**
 * Convierte coordenadas (x,y) a índice lineal en el tablero
 * @param x Coordenada X
 * @param y Coordenada Y  
 * @param width Ancho del tablero
 * @return Índice para acceder a board[idx]
 */
static inline int idx(int x, int y, int width) {
    return y * width + x;
}

/**
 * Verifica si las coordenadas están dentro del tablero
 * @param x Coordenada X
 * @param y Coordenada Y
 * @param width Ancho del tablero
 * @param height Alto del tablero
 * @return true si está dentro, false caso contrario
 */
static inline bool is_inside(int x, int y, int width, int height) {
    return (x >= 0 && x < width && y >= 0 && y < height);
}

/**
 * Verifica si una celda está libre (contiene recompensa)
 * @param value Valor de la celda
 * @return true si está libre (valor 1-9), false si está capturada (<=0)
 */
static inline bool cell_is_free(int value) {
    return (value >= MIN_REWARD && value <= MAX_REWARD);
}

/**
 * Verifica si una celda está capturada por un jugador
 * @param value Valor de la celda
 * @return true si está capturada (valor negativo), false caso contrario
 */
static inline bool cell_is_captured(int value) {
    return (value <= 0);
}

/**
 * Obtiene el ID del jugador que capturó una celda
 * @param value Valor de la celda (debe ser negativo)
 * @return ID del jugador (0-based), -1 si la celda no está capturada
 */
static inline int get_cell_owner(int value) {
    return cell_is_captured(value) ? (-value) : -1;
}

/**
 * Convierte un ID de jugador a valor de celda capturada
 * @param player_id ID del jugador (0-based)
 * @return Valor negativo para marcar la celda como capturada
 */
static inline int player_to_cell_value(int player_id) {
    return -player_id;
}

/**
 * Obtiene el desplazamiento (dx, dy) para una dirección dada
 * @param dir Dirección (0-7)
 * @param dx Puntero para almacenar desplazamiento X
 * @param dy Puntero para almacenar desplazamiento Y
 */
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

/**
 * Verifica si una dirección es válida (0-7)
 * @param dir Dirección a verificar
 * @return true si es válida, false caso contrario
 */
static inline bool is_valid_direction(unsigned char dir) {
    return (dir <= 7);
}

/**
 * Calcula el tamaño total de la estructura game_t incluyendo el tablero
 * @param width Ancho del tablero
 * @param height Alto del tablero
 * @return Tamaño en bytes de la estructura completa
 */
static inline size_t game_state_size(int width, int height) {
    return sizeof(game_state_t) + (width * height * sizeof(int));
}

#endif // COMMON_H