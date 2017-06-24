#ifndef VIEW_H
#define VIEW_H
#include <SDL/SDL.h>

typedef struct {
        unsigned int width;
        unsigned int height;
        char * data;
        char * mem_ptr;
        SDL_Surface * sdl_surface;
} Image;

typedef struct {
        unsigned int width;
        unsigned int height;
        SDL_Surface * screen;
} Viewer;

void viewsys_init();
void viewsys_wait(unsigned int msec);
void viewsys_quit();
Viewer *view_open(unsigned int width, unsigned int height, 
			const char * title);

void view_close(Viewer * view);
void view_disp_image(Viewer * view, Image * img);
Image * imgNew(unsigned int width, unsigned int height);
void imgDestroy(Image * img);
void convertYUV2RGB(unsigned char *pY, unsigned char *pU, unsigned char *pV, int width, int height, Image *pI);


#endif

