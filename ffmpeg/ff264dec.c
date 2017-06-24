/*
 * H.264 Network packet decoder
 *
 * Referring  FFMPEG decoder_encoder.c
 *
 * no real time decoding of 2K H264 in RPi 
 * 
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h> 
#include <pthread.h>
//extern "C" {
#ifndef UINT64_C
#define UINT64_C(c) (c ## ULL)
#endif

#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavcodec/avcodec.h"
#include "libavutil/mathematics.h"
#include "libavutil/samplefmt.h"
#include "libavformat/avformat.h"
#include "unistd.h"
//}

#include "ff264dec.h"

/*===========================================================================*/
/* LOCAL GLOBALs                                                             */
/*===========================================================================*/
//static unsigned char oneframebuffer[1024*128];
static bool isCodecActive = false;
static int nframe = 0;
static AVCodecContext *codecCtx = NULL;	// codec instance
static AVPacket avpkt;			// wrapper for encoded data handshaking
static AVFrame *picture = NULL;		// ouput picture

//2016.Jul26
struct frameData {
	int _width;
	int _height;
	AVFrame _picture;
	unsigned char _pGray[8];
	int _linesize;
};
/*struct frameData {
	int _width;
	int _height;
	AVFrame *_picture;
	unsigned char *_pGray;
	int _linesize;
};*/

/*===========================================================================*/
/* EXTPORT FUNCs                                                                  */
/*===========================================================================*/
/*
 * save RGB raw image into file for verifying decoding result
 *
 */

static void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize,
		char *filename) {
	FILE *f;
	int i;

	f = fopen(filename, "w");
	fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
	for (i = 0; i < ysize; i++)
		fwrite(buf + i * wrap, 1, xsize, f);
	fclose(f);
}

/**
 *
 * Initialize the codec context
 *
 * Single tone (why not!)
 *
 *
 * */

int H264DecoderInit() {
	static bool inited = false;
	AVCodec *codec;

	if (!inited) {     	// only once for program
		inited = true;
		av_register_all();
		avcodec_register_all();
	}

	av_init_packet(&avpkt);
	/* set end of buffer to 0 (this ensures that no overreading happens for damaged mpeg streams) */
	//memset(inbuf + INBUF_SIZE, 0, FF_INPUT_BUFFER_PADDING_SIZE);
	/* find the mpeg1 video decoder */
	codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		fprintf(stderr, "codec not found\n");
		exit(1);
	}

	codecCtx = avcodec_alloc_context3(codec);

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55,28,1)
	picture = av_frame_alloc();
#else
	picture = avcodec_alloc_frame();

#endif
	if (codec->capabilities & CODEC_CAP_TRUNCATED)
		codecCtx->flags |= CODEC_FLAG_TRUNCATED; /* we do not send complete frames */

	/* open it */
	if (avcodec_open2(codecCtx, codec, NULL) < 0) {
		fprintf(stderr, "could not open codec\n");
		exit(1);
	}

	isCodecActive = true;

	return 0; // OK

}

/*
 * emitVideoFrame (H.264 only)
 *
 * inbuf : one or multiple NALs for one frame
 *         Start Pattern (0x00, 0x00, 0x00, 0x01) needed
 * len   : size of in buf
 * toSave: save or not / process or not
 *
 * return : number of decoded frame
 */

#if 0 
void *vision_thread_start(void *arg) {

	pthread_t self = pthread_self();
	pthread_detach(self);
	
	//struct frameData *tinfo = *(struct frameData *) malloc(sizeof (*arg)); refer: http://stackoverflow.com/questions/19404040/void-is-not-a-pointer-to-object-type
	struct frameData * tinfo = static_cast<struct frameData*>(arg);
	int err;
	printf("Go to thread\n");
	isBusyVision = true;
	
    if (tinfo->_pGray != NULL) {
		//printf("\n Frame is not empty\n");
		
		err = onVisionFrame(tinfo->_pGray, tinfo->_linesize, tinfo->_width,
				tinfo->_height, &(tinfo->_picture));
	} else
		printf("\n Frame is empty\n");
        
        printf("[VISION THREAD] **************************VISION*******\n");
	isBusyVision = false;
//	 double visionPeriod = calculatePeriodOfTime(visionStartTime); //ms
//	std::cout<<"End thread, processing time = \n"<< visionPeriod<<std::endl;
	std::cout<<"********End VISON thread******"<<std::endl;
	pthread_exit(NULL);
}
#endif


int H264DecoderDecode(unsigned char *inbuf, int len, bool toSave, 
				void *p) 
{
	struct timeval videoDecode;
        double timeDecode;
        cb_func_t pcb_func = (cb_func_t)p;
        gettimeofday(&videoDecode, 0);
        
	int got_picture, res;
	struct frameData fd;
	nframe++;
	// 1 setup input data buffer
	avpkt.data = inbuf;
	avpkt.size = len;

	// 2. decode
	res = avcodec_decode_video2(codecCtx, picture, &got_picture, &avpkt);
	if (res < 0) {
		fprintf(stderr, "Error while decoding frame %d\n", nframe);
		return -1;
		//exit(1);
	}
	/*FILE *pFileDecode;
	 clock_t tEndDecode = clock();
	 double exTimeDecode = (double) (tEndDecode - tStartDecode) / CLOCKS_PER_SEC;
	 pFileDecode = fopen("SaveData/resultDecode.txt", "a");
	 if (pFileDecode == NULL) {
	 perror("Error opening file.");
	 } else
	 fprintf(pFileDecode, "Decode: executable time= %.6fs \n", exTimeDecode);
	 fclose(pFileDecode);*/
	/*clock_t tEndDecode = clock();
	double exDecodeTime = (double) (tEndDecode - tStartDecode) / CLOCKS_PER_SEC;
	 */
	// 3 check if gotten decoded frame
	if (got_picture) {
		char filename[128];

		if(pcb_func)
	         (*pcb_func)( picture->data[0],
			      picture->data[1],
			      picture->data[2], 
			      codecCtx->width, 
			      codecCtx->height);

		if (toSave) {

#if 1 
			printf("saving frame %3d\n", nframe);
			fflush(stdout);
			snprintf(filename, sizeof(filename), "recimg%03d.pgm", nframe);
			pgm_save(picture->data[0],   // data for YUV ?
					picture->linesize[0], codecCtx->width, codecCtx->height, // resolution
					filename);
#endif

		} else {
			/*onVisionFrame(picture->data[0],   // data for YUV ?
			 picture->linesize[0], codecCtx->width, codecCtx->height);//AVFrame *picture*/
			//cong.anh
			/*unsigned char* arg = new unsigned char[3];
			 arg[0] = codecCtx->width; //4 byte
			 arg[1] = codecCtx->height //4 byte;
			 //arg[2] copy data cua picture*/

			/*fd._linesize = picture->linesize[0];
			 fd._width = codecCtx->width;
			 fd._height = codecCtx->height;
			 fd._pGray = picture->data[0];
			 fd._picture = picture;
			*/
                   
			fd._linesize = picture->linesize[0];
			fd._width = codecCtx->width;
			fd._height = codecCtx->height;
			memcpy(fd._pGray, picture->data, 8);
			memcpy(&fd._picture,picture,sizeof(AVFrame));
                        
#if 0 
                        //Estimate video decodeing time
                        timeDecode = calculatePeriodOfTime(videoDecode);
                        logVideoDecodeStep(timeDecode);

			if(isAutoMode)
			{
				if (isBusyVision == false) {
					gettimeofday(&visionStartTime, 0);
					pthread_t t;
					pthread_create(&t, NULL, &vision_thread_start, &fd);
				}
			}
                  /*  if (isBusyVision == false)
                    {
			onVisionFrame(
			 picture->data[0],   // data for YUV ?
			 picture->linesize[0], codecCtx->width, codecCtx->height,
			 picture);
                    }
		*/	 
#endif

		}

		return 1;
	}
	// if multiple frames for one packet
	// avpkt.size -= len;  avpkt.data += len;

	return 0;

	/* some codecs, such as MPEG, transmit the I and P frame with a
	 latency of one frame. You must do the following to have a
	 chance to get the last frame of the video */

	// Found Bebop doesnot use B picture, Fortunately !! for Low delay
}

int H264DecoderClose() {
	if (!isCodecActive) {
		fprintf(stderr, "Codec Close Request in inactive\n");
		return -1;
	}
	isCodecActive = false;
	avcodec_close(codecCtx);
	av_free(codecCtx);
	av_free(picture);

	return 0; // ok
}

/**
 * Test code: to see NAL Format
 * frame : a NAL
 */
static int printNALFrame(unsigned char *frame, int len) {
	static int count = 0;
	int i = 0;

	printf("%03d (len=%d):", ++count, len);
	switch (frame[4] & 0x1F) {

	case 1:
		printf("[%-3s]", "PoB");
		break;
	case 2:
	case 3:
	case 4:
		printf("[%-3s]", "PAT");
		break;
	case 5:
		printf("===========================================================\n");
		printf("[%-3s]", "IDR");

		break;
	case 6:
		printf("[%-3s]", "SEI");
		break;
	case 7:
		printf("[%-3s]", "SPS");
		break;
	case 8:
		printf("[%-3s]", "PPS");
		break;
	default:
		break;
	}

	for (i = 0; i < 10 && i < len; i++)
		printf("%02X:", frame[i]);

	printf("\n");
	return 0;
}



