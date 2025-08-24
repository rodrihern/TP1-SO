#pragma once
#include "common.h"

/* helpers lectores */
static inline void reader_enter(game_sync_t *s) {
    sem_wait(&s->writer_mutex); // Espera hasta que no haya ningún escritor esperando o un lector entrando 
    sem_wait(&s->reader_count_mutex); // Cierra el "candado" para evitar que 2 lectores a la vez modifiquen reader_count
    if (++s->reader_count == 1) // Si soy el primer lector, tomo el mutex del estado -> evita que entre un escritor mientras hayan lectores activos
        sem_wait(&s->state_mutex);
    sem_post(&s->reader_count_mutex); // Abro el "candado" para que otros lectores puedan modificar reader_count
    sem_post(&s->writer_mutex); // Libero el mutex del escritor para que otros lectores puedan entrar 
}
static inline void reader_exit(game_sync_t *s) {
    sem_wait(&s->reader_count_mutex); // Cierro el "candado" para evitar que 2 lectores a la vez modifiquen reader_count
    if (--s->reader_count == 0) // Si soy el último lector, libero el mutex del estado -> permite que un escritor entre
        sem_post(&s->state_mutex);
    sem_post(&s->reader_count_mutex); // Abro el "candado" para que otros lectores puedan modificar reader_count
}