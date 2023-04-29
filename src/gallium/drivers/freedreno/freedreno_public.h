

#ifndef FREEDRENO_PUBLIC_H
#define FREEDRENO_PUBLIC_H

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_screen;
struct renderonly;
struct sw_winsys;

struct pipe_screen *
fd_screen_create(int fd, struct renderonly *ro);

struct pipe_screen *
fd_screen_create_sw(struct sw_winsys *winsys);

#ifdef __cplusplus
}
#endif

#endif
