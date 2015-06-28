#ifndef STREAM_H
#define STREAM_H

#include <stdio.h>

#include "buffer.h"

typedef struct {
  FILE *source;
  size_t line;
  buffer buffer;
} stream;

stream *open_stream(FILE *src);
void close_stream(stream *in);
char stream_shift(stream *in);
void stream_push(stream *in, char chr);

#endif
