/* std headers   ---------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* custom header --------------------------------------------------------*/
#include "ff264dec.h"

/* local files  ---------------------------------------------------------*/
static int h264dec_file(const char *filename);
static int printNALFrame(unsigned char *frame, int len);

static FILE *yuvfptr = NULL;

/*------------------------------------------------------------------------
   The main file 
   usage:  program <h264file>
-------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	if(argc < 3)
		fprintf(stderr, "usage: %s <h264file> <yuvfile>\n", argv[0]);


	yuvfptr = fopen(argv[2], "wb");
	if(yuvfptr == NULL){
		fprintf(stderr,"Cannot open the yuvfile\n");
		return 0;
	}	 

	h264dec_file(argv[1]); 

	fclose(yuvfptr);
	return 0;
}


/*------------------------------------------------------------------------
  
  save the yuv 420 format file 
  multiple

*-------------------------------------------------------------------------*/
void save_yuv(char *pY, char *pU, char *pV, int width, int height)
{

  static int n = 0;

  //printf("YUV WRITE!!!!!\n");
 
  if(n == 0){
	printf("video resolution: %dx%d\n", width, height);
  }

  if(n++ < 20){ // for fear of overflow of file system 
  	fwrite(pY, 1, width*height, yuvfptr);
  	fwrite(pU, 1, width*height/4, yuvfptr);
  	fwrite(pV, 1, width*height/4, yuvfptr);
  }
	
  return 0;
}

/*-------------------------------------------------------------------------
  NAL Packet 
  
  H264 video data use NAL (network adaptatio Layer) format)

  H264Video = {Delim NAL}* 
  Delim  = 0x00 0x00 0x00 0x01
  NAL    = NALH (1B) +  vriable length data

--------------------------------------------------------------------------*/

/* ------------------------------------------------------------------------
   Check if NAL packet starts here
  
   p : data buffer
   return : 1 if start-of-NAL, 0 o.w.

--------------------------------------------------------------------------*/
static inline int isNalHeader(unsigned char const *p)
{
	return (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1); 
}



/*--------------------------------------------------------------------------
   Check NAL types

   Codec should start with GLOBAL Setting params
   Then  IDR frames and then other frames
   so, we have to skip the first part non Global header and IDRs 

--------------------------------------------------------------------------*/
static char *NalTypes[32] = {
	"0", "P/B", "PAT", "PAT", "PAT", "IDR", "SEI", "SPS", 
	"PPS", "?",   "?",   "?",   "?",   "?",   "?",   "?", 
  	"?", "?",   "?",   "?",   "?",   "?",   "?",   "?", 
  	"?", "?",   "?",   "?",   "?",   "?",   "?",   "?" };

static int printNALFrame(unsigned char *frame, int len) 
{
	static int count = 0;
        int i = 0;

	printf("%03d (len=%d):", ++count, len);
	switch (frame[4] & 0x1F) {

	case 1:
		printf("[%-3s]", "P/B"); // common video frame data 
		break;
	case 2:
	case 3:
	case 4:
		printf("[%3s]", "PAT");  //
		break;
	case 5:
		printf("[%3s]", "IDR");  // key frame data

		break;
	case 6:
		printf("[%3s]", "SEI");  // global codec param
		break;
	case 7:
		printf("[%3s]", "SPS");  // global codec param 
		break;
	case 8:
		printf("[%3s]", "PPS");  // global codec param 
		break;
	default:
		break;
	}

#define MAX_NAL_PRINT_LEN 10

	for (i = 0; i < MAX_NAL_PRINT_LEN && i < len; i++)
		printf("%02X:", frame[i]);

	printf("\n");
	return 0;
}


/*-------------------------------------------------------------------------
 * Test FFMPEG H264 Deocding Function using Local H264 file
 *
 * FFMEG deosnot decode NAL stream itself. so we have to decode NAL stream
 * and pass to the decoder  
 *
 * NAL formated file
 *           |
 *           v 
 * ibuf   |<--|--------------->|  
 *         ibufstart            BUFSZ 
 *           |
 *           v
 *  
 * frame |<--------------------------------------->| 
 *
 * 
 * TODO: It is not performance efficient implementated. 
 *       circular buffer could be used. Try better version yourself!
 * 
 ------------------------------------------------------------------------*/

static int h264dec_file(const char *filename) 
{

#define BUFSZ  10240
#define NBUF   4

	FILE *ifptr;   // input h264file 
        int n;         // read length

	unsigned char ibuf[BUFSZ];
	int i, ibufrdidx, ibufwridx = 0; // read from ibuf,  wr to ibuf
	int numNALs = 0;   // # of NAL in one iBuff 
	int foundNALHeader = 0;
	int prevNalType = 0, curNalType = 0;

	unsigned char frame[NBUF * BUFSZ]; // MAX
	int foffset = 0;

	// 1. open file 
	ifptr = fopen(filename, "rb");
	if(ifptr == 0){
	   fprintf(stderr,"Cannot open file: %s\n", filename);
	   return -1;
	}

	// 2. init FFMPEG decoder instance
	H264DecoderInit();

	// 3. decode one-frame-by-one-frame
	while (!feof(ifptr)) {

		// 3.1 (re)fill input buffer from file 
                // ibufwridx : the postion  to write from file  
                // ibufrdidx : the position to read  to frame 
		n = fread((char *)&ibuf[ibufwridx], 1, BUFSZ - ibufwridx, ifptr);
		if(n <= 0){
			fprintf(stderr, "No more data\n");
			break;
		}
                
		// 3.2 find one or many NALs from the buffer 
		ibufrdidx = 0; // from the start of ibuf
		numNALs = 0;
		while (1) {
			/*
			std::cout << "istart=" << istart << std::endl;
			if(istart > BUFSZ) std::cout << "error" << std::endl;
			 */

			// 3.2.1 search the end of current NAL (the next NAL start) 
			for (i = ibufrdidx, foundNALHeader= 0; i< BUFSZ-4; i++) {
			     foundNALHeader = isNalHeader(&ibuf[i]);
			     if(foundNALHeader){
			     	prevNalType = curNalType;
			     	curNalType = ibuf[i+4] & 0x1F; // type for new NAL
			     	printf("NAL:%s\n", NalTypes[curNalType]);
				break; // when use up all buffer data
			     }
			}

			// merge with new NAL (rdidx~ i) 
			memcpy(&frame[foffset], &ibuf[ibufrdidx], i - ibufrdidx); 
			foffset += (i-ibufrdidx);

			if(foundNALHeader){  // 1) a Complet new NAL 

				numNALs++;
				// 1) process the old NAL
				// ffmpeg needs SPP,PPS in one frame
				if(prevNalType == 1 || prevNalType == 5){ 

					//printNALFrame(frame, offset);
					// *** Decode one frame *** 
					H264DecoderDecode(frame, foffset, 0, save_yuv);
					foffset = 0;// done with the old NAL 
				}
				// 2) handle next NAL 
				//    copy nal headers part to frame_buffer
				memcpy(&frame[foffset], &ibuf[i], 4);
				foffset += 4;
				ibufrdidx = i + 4;

			} else { // 2) reaming incomplete NAL 
				if(numNALs == 0){ // TODO: this is possible bug. if NAL is larger than this buffer!
					;
				}
				memmove(ibuf, &ibuf[i], BUFSZ-i); // shift(not memcopy!!) 
				ibufwridx = BUFSZ - i; // pos to write in ibuf
				break;
			}

		} // while for NAL decoding  

	} //while for reading data from h264 file


	// 4. finish 
	fclose(ifptr);
	// destroy the instance
	H264DecoderClose();

	return 0;
}

