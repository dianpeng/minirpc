#ifndef CONF_H_
#define CONF_H_
#include <assert.h>
#include <stdio.h>
#define CAST(t,p) ((t)(p))

#ifndef NDEBUG
#define VERIFY assert
#else
#define VERIFY(cond) \
    do { \
    if( !(cond) ) { \
    fprintf(stderr,#cond); \
    abort(); \
    } \
    } while(0)
#endif /* NDEBUG */

#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif /* MAX */

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif /* MIN */

#endif /* CONF_H_ */