#ifndef ANTI_RT_SIGNAL_H
#define ANTI_RT_SIGNAL_H

#include <stdint.h>

/* The two signals a console sends, under the numbers C gives them. A
   Windows program has no others of this kind. */
#define SIGINT_SIGNAL 2
#define SIGBREAK_SIGNAL 21

/* Register the function that runs when the program receives sig. One
   function per signal, and a later call replaces the one before it. */
void anti_rt_on_signal(int64_t sig, void (*f)(int64_t));

/* The signal that arrived since the last call, or 0. The call clears
   it, so a loop reads each signal once. */
int64_t anti_rt_signal_pending(void);

#endif
