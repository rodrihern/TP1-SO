#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "common.h"
#include "shm.h"

/* ------------------------------------------------------------------ */
/*     Representación interna del "handle" a una región compartida     */
/* ------------------------------------------------------------------ */
struct shm_cdt {
    char   *name;    // copia del nombre (para unlink)
    int     fd;      // descriptor de archivo devuelto por shm_open
    size_t  size;    // tamaño mapeado actual
    void   *base;    // dirección base del mmap (NULL si no está mapeado)
    bool    owner;   // true si este proceso la creó (O_CREAT|O_EXCL) (solo el master puede crear y eliminar, por eso necesitamos este dato)
};

/* ------------------------------ helpers ----------------------------- */
static int ensure_size(int fd, size_t sz) { //Fijar tamaño real con ftruncate de shm
    return ftruncate(fd, (off_t)sz); // 0 OK, -1 error
}

//Mapea la SHM con lectura/escritura y compartida entre proceso. Devuelve el puntero a la zona de memoria
static void *map_rw(int fd, size_t sz) { 
    void *p = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    return (p == MAP_FAILED) ? NULL : p; //devuelve un puntero a la memoria compartida recién mapeada. Si falla, devuelve NULL.
}

//Desmapea si hay algo mapeado. Útil cuando necesitamos remapear con otro tamaño.
// Devuelve: 0 (ok) o -1 (error)
static int unmap_if_mapped(struct shm_cdt *r) { 
    if (r->base && r->size) { // Si hay algo mapeado...
        if (munmap(r->base, r->size) == -1)  // desmapea 
            return -1;
        r->base = NULL;
    }
    return 0;
}

// Libera todos los recursos asociados a un handle de SHM
static void free_shm_handle(struct shm_cdt *r) {
    if (r) {
        free(r->name);
        free(r);
    }
}

static int init_game_sync_semaphores(game_sync_t *sync){
    // Semáforos anónimos COMPARTIDOS ENTRE PROCESOS: pshared=1 (clave)
    // A / B: master <-> vista
    //A: “hay cambios, imprimí”.
    //B: “ya imprimí”.
    //Inician en 0 (cerrados).
    if (sem_init(&sync->view_ready,         1, 0) == -1) 
        return -1; // A
    if (sem_init(&sync->view_done,          1, 0) == -1) 
        return -1; // B
    // Lectores / Escritor: C, D, E + F
    //C/D/E + F: patrón lectores‑escritor sin inanición del escritor (lo pide el enunciado).
    //reader_count arranca en 0.
    //reader_count_mutex protege a F.
    //state_mutex/writer_mutex los usás para que jugadores (lectores) convivan y el máster (escritor) no se quede con hambre. (La versión exacta la definís vos, pero debe prevenir inanición del escritor.)
    if (sem_init(&sync->writer_mutex,       1, 1) == -1) 
        return -1; // C
    if (sem_init(&sync->state_mutex,        1, 1) == -1) 
        return -1; // D
    if (sem_init(&sync->reader_count_mutex, 1, 1) == -1) 
        return -1; // E
    sync->reader_count = 0;                                          // F
    // G[i]: un semáforo por jugador, inicialmente cerrado
    // G[i]: cada jugador solo puede enviar un movimiento cuando el máster lo habilita con sem_post(G[i]). Inician cerrados. Esto también está especificado en el enunciado.
    // Importante: pshared = 1 en todos los sem_init porque los sem_t están en SHM y deben ser visibles entre procesos (no threads del mismo proceso).
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (sem_init(&sync->player_ready[i], 1, 0) == -1) 
            return -1; // G[i]
    }
    return 0;
}

/* =================================================================== */
/*                      API genérica de región SHM                      */
/* =================================================================== */

// Crea/abre una región de memoria compartida (SHM) y devuelve un “handle” vía *pr. Retorna 0 si salió bien, -1 si hubo error (y deja errno seteado).
int shm_region_open(shm_adt *pr, const char *name, size_t size) {
    if (!pr || !name || size == 0) { //Validaciones de argumentos.
        errno = EINVAL; 
        return -1; 
    } 

    struct shm_cdt *r = calloc(1, sizeof(*r)); //Creamos el handle inicializado en 0 (owner=false, fd=0, etc).
    if (!r) // Si falló la reserva, corta con error (quedará errno que haya puesto calloc).
        return -1;

    r->name = strdup(name); // Guardamos una copia del name (lo vamos a usar en unlink)
    if (!r->name) {  //Si falla la copia del nombre, libera el handle y retorna error.
        free(r); 
        return -1;
    }

    // Intento crear la shm. Si ya existe, falla con EEXIST
    // Permisos 0600 (lectura/escritura solo para el dueño).
    r->fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);

    if (r->fd != -1) { // Caso 1: Si la creación salió bien, somos dueños
        r->owner = true;
        r->size  = size;
        if (ensure_size(r->fd, size) == -1) { // Ajusta el tamaño real del objeto SHM con ftruncate (dentro de ensure_size).
            int e = errno;
            close(r->fd);
            shm_unlink(name);
            free_shm_handle(r);
            errno = e;
            return -1; 
        }
    } else if (errno == EEXIST) { // Caso 2: Si falló porque ya existía. Solo abrir
        r->fd = shm_open(name, O_RDWR, 0600);

        if (r->fd == -1) {
            int e = errno;
            free_shm_handle(r);
            errno = e;
            return -1;
        }
        //Descubrimos el tamaño real existente (clave cuando no somos dueños).
        //Guardamos owner=false.
        struct stat st;
        if (fstat(r->fd, &st) == -1) {
            int e = errno;
            close(r->fd); 
            free_shm_handle(r);
            errno = e;
            return -1;
        }
        r->size  = (size_t)st.st_size; // uso el tamaño existente
        r->owner = false;
    } else { // Caso 3: Si falló por otro motivo, corta con error.
        int e = errno;
        free_shm_handle(r);
        errno = e;
        return -1;
    }

    //Dejamos sin mapear por ahora (se mapea en las funciones game_state_map y game_sync_map, porque cada una sabe qué tamaño y que struct necesita).
    r->base = NULL;
    *pr = r;
    return 0;
}

//Cierra la región (para procesos que no son dueños o cuando no queremos destruirla).
//Desmapea si hacía falta, cierra el fd y libera el handle.
int shm_region_close(shm_adt r_) {
    if (!r_) { 
        errno = EINVAL; 
        return -1; 
    }
    struct shm_cdt *r = (struct shm_cdt*)r_; // AZU: NO ENTIENDO POR QUÉ SE HACE ESTO

    int rc = 0;
    if (unmap_if_mapped(r) == -1) 
        rc = -1;
    if (close(r->fd) == -1) 
        rc = -1;

    free_shm_handle(r);
    return rc;
}

//Hace shm_unlink solo si este proceso fue el creador.
//Ojo: unlink no desmapea ni cierra; por eso existe además *_unmap_destroy.
int shm_region_unlink_if_owner(shm_adt r_) {
    if (!r_) { 
        errno = EINVAL; 
        return -1; 
    }
    struct shm_cdt *r = (struct shm_cdt*)r_;
    if (!r->owner) 
        return 0;
    return shm_unlink(r->name);
}

/* =================================================================== */
/*                Mapeos específicos del estado y la sync               */
/* =================================================================== */
//Calcula el tamaño requerido incluyendo el array flexible board[].
int game_state_map(shm_adt r_, unsigned short w, unsigned short h, game_state_t **out) {
    if (!r_ || !out || w == 0 || h == 0) { 
        errno = EINVAL; 
        return -1; 
    }
    struct shm_cdt *r = (struct shm_cdt*)r_;

    size_t need = game_state_size(w, h); // sizeof(game_state_t) + w*h*sizeof(int)

    // Si soy owner y no alcanza el tamaño actual, lo agrando
    //Si no somos dueños y el tamaño existente es menor al que pedimos, error (la región creada por el máster no coincide con w*h).
    if (r->owner && r->size < need) {
        if (ensure_size(r->fd, need) == -1) 
            return -1;
        r->size = need;
    }

    // (Re)mapear si hace falta
    if (!r->base || r->size < need) {
        if (unmap_if_mapped(r) == -1) 
            return -1;
        if (!r->owner && r->size < need) { 
            errno = EINVAL; 
            return -1; 
        } // región muy chica
        r->base = map_rw(r->fd, r->size);
        if (!r->base) 
            return -1;
    }

    //Si somos dueños, inicializamos todo en cero (incluido board) y seteamos dimensiones.
    //El llenado con recompensas 1..9 se hace después, por la lógica del máster (para poder usar la seed). (El enunciado fija 1..9 para celdas libres).
    game_state_t *gs = (game_state_t*)r->base;
    *out = gs;

    if (r->owner) {
        // Inicialización mínima segura. Llenar el board con 1..9 lo hace el máster luego.
        memset(gs, 0, r->size);
        gs->board_width   = w;
        gs->board_height  = h;
        gs->num_players   = 0;
        gs->game_finished = false;
    }
    return 0;
}

///game_sync siempre vale sizeof(game_sync_t) (no hay array flexible).
int game_sync_map(shm_adt r_, game_sync_t **out) {
    if (!r_ || !out) { 
        errno = EINVAL; 
        return -1; 
    }
    struct shm_cdt *r = (struct shm_cdt*)r_;

    size_t need = sizeof(game_sync_t);

    if (r->owner && r->size < need) {
        if (ensure_size(r->fd, need) == -1) 
            return -1;
        r->size = need;
    }

    //Mapeo normal (mismo patrón que game_state_map).
    if (!r->base || r->size < need) {
        if (unmap_if_mapped(r) == -1) 
            return -1;
        if (!r->owner && r->size < need) { 
            errno = EINVAL; 
            return -1; 
        }
        r->base = map_rw(r->fd, r->size);
        if (!r->base) 
            return -1;
    }

    game_sync_t *sync = (game_sync_t*)r->base;
    *out = sync;

    if (r->owner) {
        if (init_game_sync_semaphores(sync) == -1)
            return -1;
    }

    return 0;
}

/* =================================================================== */
/*               Helpers "todo en uno" de destrucción final            */
/* =================================================================== */
//Para /game_state no hay semáforos: solo desmapeamos, cerramos y (si corresponde) unlink
int game_state_unmap_destroy(shm_adt r_) {
    if (!r_) { 
        errno = EINVAL; 
        return -1; 
    }
    struct shm_cdt *r = (struct shm_cdt*)r_;
    int rc = 0;

    if (unmap_if_mapped(r) == -1) 
        rc = -1;
    if (close(r->fd) == -1) 
        rc = -1;
    if (r->owner && shm_unlink(r->name) == -1) 
        rc = -1;
    free_shm_handle(r);
    return rc;
}

//Orden correcto cuando hay semáforos en SHM:
//1.sem_destroy (mientras la memoria está mapeada),
//2.munmap,
//3.close,
//4.shm_unlink (solo el dueño).
//Esto evita leaks y cumple con la limpieza de recursos que piden.
int game_sync_unmap_destroy(shm_adt r_) {
    if (!r_) { 
        errno = EINVAL; 
        return -1; 
    }
    struct shm_cdt *r = (struct shm_cdt*)r_;
    int rc = 0;

    // Sólo el creador destruye semáforos (orden: destruir -> unmap -> close -> unlink)
    if (r->owner && r->base) {
        game_sync_t *sync = (game_sync_t*)r->base;
        int e = 0;
        e |= sem_destroy(&sync->view_ready);
        e |= sem_destroy(&sync->view_done);
        e |= sem_destroy(&sync->writer_mutex);
        e |= sem_destroy(&sync->state_mutex);
        e |= sem_destroy(&sync->reader_count_mutex);
        for (int i = 0; i < MAX_PLAYERS; ++i) 
            e |= sem_destroy(&sync->player_ready[i]);
        if (e == -1) 
            rc = -1;
    }

    if (unmap_if_mapped(r) == -1) 
        rc = -1;
    if (close(r->fd) == -1) 
        rc = -1;
    if (r->owner && shm_unlink(r->name) == -1) 
        rc = -1;

    free_shm_handle(r);
    return rc;
}
