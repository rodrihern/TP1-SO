#ifndef WRITER_SYNC_H
#define WRITER_SYNC_H

#pragma once
#include "common.h"

static inline void writer_enter(game_sync_t *s) {
    sem_wait(&s->writer_mutex);
    sem_wait(&s->state_mutex); 
}

static inline void writer_exit(game_sync_t *s) {
    sem_post(&s->state_mutex); 
    sem_post(&s->writer_mutex); 
}

#endif