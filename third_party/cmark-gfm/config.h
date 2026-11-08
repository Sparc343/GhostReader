#ifndef CMARK_CONFIG_H
#define CMARK_CONFIG_H
#ifdef __cplusplus
extern "C" {
#endif
#define HAVE_STDBOOL_H
#include <stdbool.h>
#define HAVE___BUILTIN_EXPECT
#define HAVE___ATTRIBUTE__
#define CMARK_ATTRIBUTE(list) __attribute__ (list)
#ifndef CMARK_INLINE
  #define CMARK_INLINE inline
#endif
#ifdef __cplusplus
}
#endif
#endif
