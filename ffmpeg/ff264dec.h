#ifndef FF264DEC_H
#define FF264DEC_H
//extern "C"
//{

enum _bool {
	false = 0, 
	true = 1
};
typedef enum _bool bool;

// call back function when it decoded one frame 
typedef void (*cb_func_t)(unsigned char *y, unsigned char *u, unsigned char *v, int w, int h);

// init ffmpeg decoder: do all the details  
int H264DecoderInit(); 

// decode one or multile NALs without delimits
int H264DecoderDecode(unsigned char *inbuf, int len, bool toSave, void *pcbf);

// free the codec resource
int H264DecoderClose(); 

//}
#endif
