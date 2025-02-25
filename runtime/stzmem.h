#ifndef STANZA_STZMEM_H
#define STANZA_STZMEM_H

void* stz_malloc (stz_long size);
void stz_free (void* ptr);
void* stz_realloc (void* ptr, stz_long new_size);

#endif
