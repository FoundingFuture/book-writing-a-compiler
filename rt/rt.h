#ifndef ANTI_RT_H
#define ANTI_RT_H

/* The state of the runtime. An executable initialises it in main. A shared
   library initialises it in a constructor that runs when it loads. A
   static library for C initialises it on the first use of the runtime. */
void anti_rt_init(void);

/* 1 after anti_rt_init, else 0. */
int anti_rt_ready(void);

#endif
