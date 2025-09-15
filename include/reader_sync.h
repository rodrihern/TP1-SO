#ifndef READER_SYNC_H
#define READER_SYNC_H

#pragma once
#include "common.h"


static inline void reader_enter(game_sync_t *s) {
    sem_wait(&s->writer_mutex); 
    sem_wait(&s->reader_count_mutex); 
    if (++s->reader_count == 1) 
        sem_wait(&s->state_mutex);
    sem_post(&s->reader_count_mutex); 
    sem_post(&s->writer_mutex); 
}

static inline void reader_exit(game_sync_t *s) {
    sem_wait(&s->reader_count_mutex);
    if (--s->reader_count == 0) 
        sem_post(&s->state_mutex);
    sem_post(&s->reader_count_mutex);
}

#endif