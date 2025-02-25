#include "stzmem.h"

void* stz_malloc (stz_long size){
  void* result = malloc(size);
  if(!result){
    fprintf(stderr, "FATAL ERROR: Out of memory.");
    exit(-1);
  }
  return result;
}

void stz_free (void* ptr){
  free(ptr);
}

void* stz_realloc (void* ptr, stz_long new_size){
  return realloc(ptr, new_size);
}
