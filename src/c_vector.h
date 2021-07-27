
#ifndef C_VECTOR_HEADER
#define C_VECTOR_HEADER

#include <stdint.h>
#include <stdlib.h>

#define vector_roundup32(x)                                                   \
  (--(x), (x) |= (x) >> 1, (x) |= (x) >> 2, (x) |= (x) >> 4, (x) |= (x) >> 8, \
   (x) |= (x) >> 16, ++(x))

#define vector_t(type) \
  struct {             \
    int32_t n, m;      \
    type *a;           \
  }
#define vector_init(v) ((v)->n = (v)->m = 0, (v)->a = 0)
#define vector_destroy(v) free((v)->a)
#define vector_at(v, i) ((v)->a[(i)])
#define vector_pop(v) ((v)->a[--(v)->n])
#define vector_size(v) ((v)->n)
#define vector_max(v) ((v)->m)

#define vector_resize(type, v, s) \
  ((v)->m = (s), (v)->a = (type *)realloc((v)->a, sizeof(type) * (v)->m))

#define vector_push(type, v, x)                                \
  do {                                                         \
    if ((v)->n == (v)->m) {                                    \
      (v)->m = (v)->m ? (v)->m << 1 : 2;                       \
      (v)->a = (type *)realloc((v)->a, sizeof(type) * (v)->m); \
    }                                                          \
    (v)->a[(v)->n++] = (x);                                    \
  } while (0)

#define vector_pushp(type, v)                                         \
  (((v)->n == (v)->m)                                                 \
       ? ((v)->m = ((v)->m ? (v)->m << 1 : 2),                        \
          (v)->a = (type *)realloc((v)->a, sizeof(type) * (v)->m), 0) \
       : 0),                                                          \
      ((v)->a + ((v)->n++))

#define vector_a(type, v, i)                                          \
  ((v)->m <= (i)                                                      \
       ? ((v)->m = (v)->n = (i) + 1, vector_roundup32((v)->m),        \
          (v)->a = (type *)realloc((v)->a, sizeof(type) * (v)->m), 0) \
       : (v)->n <= (i) ? (v)->n = (i) : 0),                           \
      (v)->a[(i)]

#endif  // C_VECTOR_HEADER
