#include <malloc.h>
#include <SDL/SDL.h>
#include "view.h"

// initialise
void viewsys_init()
{
  SDL_Init(SDL_INIT_VIDEO);
}

// delay for a given number of milliseconds
void viewsys_wait(unsigned int msec)
{
  SDL_Delay(msec);
}

// quit
void viewsys_quit()
{
  SDL_Quit();
}

Viewer *view_open(unsigned int width, unsigned int height, 
			const char * title)
{       
   // set up the view
   Viewer *view = (Viewer *)malloc(sizeof(*view));
   if(view == NULL){
      fprintf(stderr, "Could not allocate memory for view\n");
      return NULL;
   }

   // initialise the screen surface
   view->screen = SDL_SetVideoMode(width, height, 24, SDL_SWSURFACE);
   if(view == NULL){
      fprintf(stderr, "Failed to open screen surface\n");
      free(view);
      return NULL;
   }
   // set the window title
   SDL_WM_SetCaption(title, 0);
   // return the completed view object
   return view;
}


void view_close(Viewer * view)
{
        // free the screen surface
        SDL_FreeSurface(view->screen);
        // free the view container
        free(view);
}

void view_disp_image(Viewer * view, Image * img)
{
        // Blit the image to the window surface
        SDL_BlitSurface(img->sdl_surface, NULL, view->screen, NULL);
        
        // Flip the screen to display the changes
        SDL_Flip(view->screen);
}


/* 
 * make img RGB24 surfaces 
 * @TODO: enable YUV format!
 */
Image * imgNew(unsigned int width, unsigned int height)
{
        // Allocate for the image container
        Image *img = (Image *)malloc(sizeof(*img));
        if(img == NULL){
                fprintf(stderr, "Failed to allocate mem for image container\n");
                return NULL;
        }
        img->width = width;
        img->height = height;

        // allocate image data, 3 byte per pixel, 8-Byte aligned 
        img->mem_ptr = (char *)malloc(img->width * img->height * 3 + 8);
        if(img->mem_ptr == NULL){
                fprintf(stderr, "Memory allocation of image data failed\n");
                free(img);
                return NULL;
        }

       // make certain it is aligned to 8 bytes
       unsigned int remainder = ((size_t)img->mem_ptr) % 8;
       if(remainder == 0)
          img->data = img->mem_ptr;
       else 
          img->data = img->mem_ptr + (8 - remainder);
        
        // Fill the SDL_Surface container
        img->sdl_surface = SDL_CreateRGBSurfaceFrom(
                                img->data,
                                img->width,
                                img->height,
                                24, 
                                img->width * 3,
                                0xff0000,
                                0x00ff00,
                                0x0000ff,
                                0x000000);

       // check the surface was initialised
        if(img->sdl_surface == NULL){
                fprintf(stderr, "Failed to initialise RGB surface from pixel data\n");
                SDL_FreeSurface(img->sdl_surface);
                free(img->mem_ptr);
                free(img);
                return NULL;
        }

        return img;
}


/*
 * Destroys the image
 */
void imgDestroy(Image * img)
{
  // Free the SDL surface
   SDL_FreeSurface(img->sdl_surface);
   if(img->mem_ptr != NULL)
       free(img->mem_ptr);
        
   // free the image container
   free(img);
}

/* 
 *  convert and re-scale 
 *  now only scale down to 320x240
 *  e.g.) 640x480 to 320x240
 *        1920x1080 to 320x240
 */
void convertYUV2RGB(unsigned char *pY, unsigned char *pU, unsigned char *pV, 
			int width, int height, 
					Image *pI){
   int scale = 1;
   int x, y;

   if(width == 640) 
      scale = 2;

   // Copy data across, converting to RGB along the way
   char *pRGB = pI->data;

   for(y = 0; y < 240; y++){
	for(x = 0; x < 320; x++){ 

           int dstidx = 3*(y*320 +x);
           int srcidx_y = (y*640 +x)*2; //
	   int srcidx_uv = y*320 +x;    // already subsampled
  
	   // avg of 4 pixels
           int yy = (pY[srcidx_y] + pY[srcidx_y +1] 
			+  pY[srcidx_y + 640] +  pY[srcidx_y + 641]) >> 2;
	   int uu =  pU[srcidx_uv];
	   int vv =  pV[srcidx_uv];

           // yuv to rgb
           int r = yy + ((357 * vv) >> 8) - 179;
           int g = yy - (( 87 * uu) >> 8) +  44 - ((181 * vv) >> 8) + 91;
           int b = yy + ((450 * uu) >> 8) - 226;
           // clamp to 0 to 255
           pRGB[dstidx +2] = r > 254 ? 255 : (r < 0 ? 0 : r);
           pRGB[dstidx +1] = g > 254 ? 255 : (g < 0 ? 0 : g);
           pRGB[dstidx +0] = b > 254 ? 255 : (b < 0 ? 0 : b);
     }
   } 

#if 0 
   // iterate 2 pixels at a time, so 4 bytes for YUV and 6 bytes for RGB^M
   for(int i = 0; i < pI->width*pI->height/2; 
	pY+=2*scale, pU+=scale, pV+=scale, pRGB+=6, i++){

        // YCbCr to RGB conversion 
	// (from: http://www.equasys.de/colorconversion.html);^M
        int y0 = *pY, cb = *pU, y1 = *(pY +1), cr = *pV;
        int r, g, b;
        // first RGB
        r = y0 + ((357 * cr) >> 8) - 179;
        g = y0 - (( 87 * cb) >> 8) +  44 - ((181 * cr) >> 8) + 91;
        b = y0 + ((450 * cb) >> 8) - 226;
        // clamp to 0 to 255
        pRGB[2] = r > 254 ? 255 : (r < 0 ? 0 : r);
        pRGB[1] = g > 254 ? 255 : (g < 0 ? 0 : g);
        pRGB[0] = b > 254 ? 255 : (b < 0 ? 0 : b);
               
        // second RGB
        r = y1 + ((357 * cr) >> 8) - 179;
        g = y1 - (( 87 * cb) >> 8) +  44 - ((181 * cr) >> 8) + 91;
        b = y1 + ((450 * cb) >> 8) - 226;
        pRGB[5] = r > 254 ? 255 : (r < 0 ? 0 : r);
        pRGB[4] = g > 254 ? 255 : (g < 0 ? 0 : g);
        pRGB[3] = b > 254 ? 255 : (b < 0 ? 0 : b);
      }
#endif


}


/*----------------------------------------------------------------------
 * demo how to use SDL  
 *
----------------------------------------------------------------------*/
int demo(int argc, char *argv[])
{
   int n = 0;
   int w = 320, h = 240;
   viewsys_init();
   Viewer *pV = view_open(w, h, "SDLViewer");
   Image *pI =  imgNew(w, h);

   for(n = 0; n < 10; n++){
     int x, y;
     int offset;
     switch(n%3){
	   case 0:
              printf("B space!\n");
	      break;
	   case 1:
              printf("G space!\n");
	      break;
	   case 2:
              printf("R space!\n");
	      break;
    }

     /* note: BGR not RGB */
     for(y=0;y<h;y++)
        for(x=0;x<w;x++){
          offset = 3*(w*y +x);
	  pI->data[offset+0] =  pI->data[offset+1] = pI->data[offset+2] = 0; 
	  switch(n%3){
	   case 0:
              pI->data[offset+0] = 200; // B
	      break;
	   case 1:
              pI->data[offset+1] = 200;  //G
	      break;
	   case 2:
              pI->data[offset+2] = 200;  //R
	      break;
	  }
	}

     view_disp_image(pV, pI);
     viewsys_wait(5000);
   }

   view_close(pV);
   viewsys_quit();
   return 0;

}


/*-------------------------------------------------------------------------
  yuvviwer

  usage:  prog  <yuvfile> width height [timeintval]  
--------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	long tintval = 0;
	int w, h; 
	FILE *fptr;
        char *pyuv, *py, *pu, *pv;
	int x, y;
	int n;


	if(argc < 4){
		fprintf(stderr,"usage: %s <yuvfile> width height [interval(ms)]\n", argv[0]);
		return 0;
	}
	
	w = atoi(argv[2]);
	h = atoi(argv[3]);
	tintval = 0L;	
	if(argc >= 5)
		tintval = atol(argv[4]);
	tintval *= 1000; 
	
	/* INIT */
   	viewsys_init();
   	Viewer *pV = view_open(w, h, "SDLViewer");
   	Image *pI =  imgNew(w, h);


	fptr = fopen(argv[1], "rb");
	if(fptr == NULL){
		fprintf(stderr,"Cannot open yuv file: %s\n", argv[1]);
		goto done2;
	}

	pyuv = malloc(w*h*3/2);
	if(pyuv == NULL){
		fprintf(stderr,"Cannot allocate yuv buffer\n");
		goto done1;
	}
	

	while(!feof(fptr)){

		// 1. read one YUV frame
		n = fread(pyuv, 1, w*h*3/2, fptr); 
		if( n < w*h*3/2)
			break;

		// YUV offset
		py = pyuv;
		pu = pyuv + w*h;
		pv = pu + w*h/4;

		/* FILL IMAGE */ /* note: BGR not RGB */
		for(y=0;y<h;y++)
        		for(x=0;x<w;x++, py++, pu++, pv++){
          			int offset = 3*(w*y +x);
				// gray output, TODO, color!
              			pI->data[offset+0] = *py;    
              			pI->data[offset+1] = *py;    
              			pI->data[offset+2] = *py;  
			}

		/* DISPLAY */
     		view_disp_image(pV, pI);
     		viewsys_wait(tintval);

	}


	free(pyuv);

done1:
	fclose(fptr);
done2:
	/* FISNISH */
   	view_close(pV);
   	viewsys_quit();
   	return 0;

}

