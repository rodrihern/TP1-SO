#ifndef WRITER_SYNC_H
#define WRITER_SYNC_H

#pragma once
#include "common.h"

static inline void writer_enter(game_sync_t *s) {
    sem_wait(&s->writer_mutex); // No deja que entren lectores nuevos en reader_enter
    sem_wait(&s->state_mutex); // Espera a que los lectores actuales terminen (si hay) -> el último lector liberó el state_mutex
}

static inline void writer_exit(game_sync_t *s) {
    sem_post(&s->state_mutex); // Permite que entren nuevos lectores/escritores
    sem_post(&s->writer_mutex); // Permite que entren nuevos lectores
}

#endif