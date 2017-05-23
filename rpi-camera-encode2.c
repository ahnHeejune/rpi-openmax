/*
 * Copyright © 2013 Tuomas Jormola <tj@solitudo.net> <http://solitudo.net>
 *
 *     $ ./rpi-camera-encode >test.h264
 *     # Press Ctrl-C to interrupt the recording...
 *     $ mkvmerge -o test.mkv test.h264
 *     $ omxplayer test.mkv
 *
 * `rpi-camera-encode` uses `camera`, `video_encode` and `null_sink` components.
 * `camera` video output port is tunneled to `video_encode` input port and
 * `camera` preview output port is tunneled to `null_sink` input port. H.264
 * encoded video is read from the buffer of `video_encode` output port and dumped
 * to `stdout`.
 *
 * Please see README.mdwn for more detailed description of this
 * OpenMAX IL demos for Raspberry Pi bundle.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <bcm_host.h>

#include <interface/vcos/vcos_semaphore.h>
#include <interface/vmcs_host/vchost.h>

#include <IL/OMX_Core.h>
#include <IL/OMX_Component.h>
#include <IL/OMX_Video.h>
#include <IL/OMX_Broadcom.h>

// Hard coded parameters
#define VIDEO_WIDTH                     1920
#define VIDEO_HEIGHT                    1080
#define VIDEO_FRAMERATE                 25
#define VIDEO_BITRATE                   10000000
#define CAM_DEVICE_NUMBER               0
#define CAM_SHARPNESS                   0                       // -100 .. 100
#define CAM_CONTRAST                    0                       // -100 .. 100
#define CAM_BRIGHTNESS                  50                      // 0 .. 100
#define CAM_SATURATION                  0                       // -100 .. 100
#define CAM_EXPOSURE_VALUE_COMPENSTAION 0
#define CAM_EXPOSURE_ISO_SENSITIVITY    100
#define CAM_EXPOSURE_AUTO_SENSITIVITY   OMX_FALSE
#define CAM_FRAME_STABILISATION         OMX_TRUE
#define CAM_WHITE_BALANCE_CONTROL       OMX_WhiteBalControlAuto // OMX_WHITEBALCONTROLTYPE
#define CAM_IMAGE_FILTER                OMX_ImageFilterNoise    // OMX_IMAGEFILTERTYPE
#define CAM_FLIP_HORIZONTAL             OMX_FALSE
#define CAM_FLIP_VERTICAL               OMX_FALSE

// Dunno where this is originally stolen from...
#define OMX_INIT_STRUCTURE(a) \
    memset(&(a), 0, sizeof(a)); \
    (a).nSize = sizeof(a); \
    (a).nVersion.nVersion = OMX_VERSION; \
    (a).nVersion.s.nVersionMajor = OMX_VERSION_MAJOR; \
    (a).nVersion.s.nVersionMinor = OMX_VERSION_MINOR; \
    (a).nVersion.s.nRevision = OMX_VERSION_REVISION; \
    (a).nVersion.s.nStep = OMX_VERSION_STEP

// Global variable used by the signal handler and capture/encoding loop
static int want_quit = 0;

// Our application context passed around
// the main routine and callback handlers
typedef struct {

    OMX_HANDLETYPE camera;
    OMX_BUFFERHEADERTYPE *camera_ppBuffer_in;
    int camera_ready;

    // 1st encoder
    OMX_HANDLETYPE encoder;
    OMX_BUFFERHEADERTYPE *encoder_ppBuffer_out;
    int encoder_output_buffer_available;  // flag for non-tunelling output port, how about tunneling to file sink ?? 

#ifndef ORIGINAL
    OMX_HANDLETYPE video_splitter;   
    OMX_HANDLETYPE resize;   

    // 2nd encoder
    OMX_HANDLETYPE encoder2;
    OMX_BUFFERHEADERTYPE *encoder_ppBuffer_out2;
    int encoder_output_buffer_available2;  // flag for non-tunelling output port, how about tunneling to file sink ?? 
   
    OMX_HANDLETYPE write_media;   
#endif

    OMX_HANDLETYPE null_sink;

    int flushed;
    FILE *fd_out;
    VCOS_SEMAPHORE_T handler_lock;
} appctx;


static int verbose_level = 1;  // @Todo 
 
// Ugly, stupid utility functions
static void say(const char* message, ...) {

    if(!verbose_level) return;

    va_list args;
    char str[1024];
    memset(str, 0, sizeof(str));
    va_start(args, message);
    vsnprintf(str, sizeof(str) - 1, message, args);
    va_end(args);
    size_t str_len = strnlen(str, sizeof(str));
    if(str[str_len - 1] != '\n') {
        str[str_len] = '\n';
    }
    fprintf(stderr, str);
}

static void die(const char* message, ...) {
    va_list args;
    char str[1024];
    memset(str, 0, sizeof(str));
    va_start(args, message);
    vsnprintf(str, sizeof(str), message, args);
    va_end(args);
    say(str);
    exit(1);
}

static void omx_die(OMX_ERRORTYPE error, const char* message, ...) {
    va_list args;
    char str[1024];
    char *e;
    memset(str, 0, sizeof(str));
    va_start(args, message);
    vsnprintf(str, sizeof(str), message, args);
    va_end(args);
    switch(error) {
        case OMX_ErrorNone:                     e = "no error";                                      break;
        case OMX_ErrorBadParameter:             e = "bad parameter";                                 break;
        case OMX_ErrorIncorrectStateOperation:  e = "invalid state while trying to perform command"; break;
        case OMX_ErrorIncorrectStateTransition: e = "unallowed state transition";                    break;
        case OMX_ErrorInsufficientResources:    e = "insufficient resource";                         break;
        case OMX_ErrorBadPortIndex:             e = "bad port index, i.e. incorrect port";           break;
        case OMX_ErrorHardware:                 e = "hardware error";                                break;
        /* That's all I've encountered during hacking so let's not bother with the rest... */
        default:                                e = "(no description)";
    }
    die("OMX error: %s: 0x%08x %s", str, error, e);
}

static void dump_event(OMX_HANDLETYPE hComponent, OMX_EVENTTYPE eEvent, OMX_U32 nData1, OMX_U32 nData2) {
    char *e;
    switch(eEvent) {
        case OMX_EventCmdComplete:          e = "command complete";                   break;
        case OMX_EventError:                e = "error";                              break;
        case OMX_EventParamOrConfigChanged: e = "parameter or configuration changed"; break;
        case OMX_EventPortSettingsChanged:  e = "port settings changed";              break;
        /* That's all I've encountered during hacking so let's not bother with the rest... */
        default:
            e = "(no description)";
    }
    say("Received event 0x%08x %s, hComponent:0x%08x, nData1:0x%08x, nData2:0x%08x",
            eEvent, e, hComponent, nData1, nData2);
}

static const char* dump_compression_format(OMX_VIDEO_CODINGTYPE c) {
    char *f;
    switch(c) {
        case OMX_VIDEO_CodingUnused:     return "not used";
        case OMX_VIDEO_CodingAutoDetect: return "autodetect";
        case OMX_VIDEO_CodingMPEG2:      return "MPEG2";
        case OMX_VIDEO_CodingH263:       return "H.263";
        case OMX_VIDEO_CodingMPEG4:      return "MPEG4";
        case OMX_VIDEO_CodingWMV:        return "Windows Media Video";
        case OMX_VIDEO_CodingRV:         return "RealVideo";
        case OMX_VIDEO_CodingAVC:        return "H.264/AVC";
        case OMX_VIDEO_CodingMJPEG:      return "Motion JPEG";
        case OMX_VIDEO_CodingVP6:        return "VP6";
        case OMX_VIDEO_CodingVP7:        return "VP7";
        case OMX_VIDEO_CodingVP8:        return "VP8";
        case OMX_VIDEO_CodingYUV:        return "Raw YUV video";
        case OMX_VIDEO_CodingSorenson:   return "Sorenson";
        case OMX_VIDEO_CodingTheora:     return "OGG Theora";
        case OMX_VIDEO_CodingMVC:        return "H.264/MVC";

        default:
            f = calloc(23, sizeof(char));
            if(f == NULL) {
                die("Failed to allocate memory");
            }
            snprintf(f, 23 * sizeof(char) - 1, "format type 0x%08x", c);
            return f;
    }
}
static const char* dump_color_format(OMX_COLOR_FORMATTYPE c) {
    char *f;
    switch(c) {
        case OMX_COLOR_FormatUnused:                 return "OMX_COLOR_FormatUnused: not used";
        case OMX_COLOR_FormatMonochrome:             return "OMX_COLOR_FormatMonochrome";
        case OMX_COLOR_Format8bitRGB332:             return "OMX_COLOR_Format8bitRGB332";
        case OMX_COLOR_Format12bitRGB444:            return "OMX_COLOR_Format12bitRGB444";
        case OMX_COLOR_Format16bitARGB4444:          return "OMX_COLOR_Format16bitARGB4444";
        case OMX_COLOR_Format16bitARGB1555:          return "OMX_COLOR_Format16bitARGB1555";
        case OMX_COLOR_Format16bitRGB565:            return "OMX_COLOR_Format16bitRGB565";
        case OMX_COLOR_Format16bitBGR565:            return "OMX_COLOR_Format16bitBGR565";
        case OMX_COLOR_Format18bitRGB666:            return "OMX_COLOR_Format18bitRGB666";
        case OMX_COLOR_Format18bitARGB1665:          return "OMX_COLOR_Format18bitARGB1665";
        case OMX_COLOR_Format19bitARGB1666:          return "OMX_COLOR_Format19bitARGB1666";
        case OMX_COLOR_Format24bitRGB888:            return "OMX_COLOR_Format24bitRGB888";
        case OMX_COLOR_Format24bitBGR888:            return "OMX_COLOR_Format24bitBGR888";
        case OMX_COLOR_Format24bitARGB1887:          return "OMX_COLOR_Format24bitARGB1887";
        case OMX_COLOR_Format25bitARGB1888:          return "OMX_COLOR_Format25bitARGB1888";
        case OMX_COLOR_Format32bitBGRA8888:          return "OMX_COLOR_Format32bitBGRA8888";
        case OMX_COLOR_Format32bitARGB8888:          return "OMX_COLOR_Format32bitARGB8888";
        case OMX_COLOR_FormatYUV411Planar:           return "OMX_COLOR_FormatYUV411Planar";
        case OMX_COLOR_FormatYUV411PackedPlanar:     return "OMX_COLOR_FormatYUV411PackedPlanar: Planes fragmented when a frame is split in multiple buffers";
        case OMX_COLOR_FormatYUV420Planar:           return "OMX_COLOR_FormatYUV420Planar: Planar YUV, 4:2:0 (I420)";
        case OMX_COLOR_FormatYUV420PackedPlanar:     return "OMX_COLOR_FormatYUV420PackedPlanar: Planar YUV, 4:2:0 (I420), planes fragmented when a frame is split in multiple buffers";
        case OMX_COLOR_FormatYUV420SemiPlanar:       return "OMX_COLOR_FormatYUV420SemiPlanar, Planar YUV, 4:2:0 (NV12), U and V planes interleaved with first U value";
        case OMX_COLOR_FormatYUV422Planar:           return "OMX_COLOR_FormatYUV422Planar";
        case OMX_COLOR_FormatYUV422PackedPlanar:     return "OMX_COLOR_FormatYUV422PackedPlanar: Planes fragmented when a frame is split in multiple buffers";
        case OMX_COLOR_FormatYUV422SemiPlanar:       return "OMX_COLOR_FormatYUV422SemiPlanar";
        case OMX_COLOR_FormatYCbYCr:                 return "OMX_COLOR_FormatYCbYCr";
        case OMX_COLOR_FormatYCrYCb:                 return "OMX_COLOR_FormatYCrYCb";
        case OMX_COLOR_FormatCbYCrY:                 return "OMX_COLOR_FormatCbYCrY";
        case OMX_COLOR_FormatCrYCbY:                 return "OMX_COLOR_FormatCrYCbY";
        case OMX_COLOR_FormatYUV444Interleaved:      return "OMX_COLOR_FormatYUV444Interleaved";
        case OMX_COLOR_FormatRawBayer8bit:           return "OMX_COLOR_FormatRawBayer8bit";
        case OMX_COLOR_FormatRawBayer10bit:          return "OMX_COLOR_FormatRawBayer10bit";
        case OMX_COLOR_FormatRawBayer8bitcompressed: return "OMX_COLOR_FormatRawBayer8bitcompressed";
        case OMX_COLOR_FormatL2:                     return "OMX_COLOR_FormatL2";
        case OMX_COLOR_FormatL4:                     return "OMX_COLOR_FormatL4";
        case OMX_COLOR_FormatL8:                     return "OMX_COLOR_FormatL8";
        case OMX_COLOR_FormatL16:                    return "OMX_COLOR_FormatL16";
        case OMX_COLOR_FormatL24:                    return "OMX_COLOR_FormatL24";
        case OMX_COLOR_FormatL32:                    return "OMX_COLOR_FormatL32";
        case OMX_COLOR_FormatYUV420PackedSemiPlanar: return "OMX_COLOR_FormatYUV420PackedSemiPlanar: Planar YUV, 4:2:0 (NV12), planes fragmented when a frame is split in multiple buffers, U and V planes interleaved with first U value";
        case OMX_COLOR_FormatYUV422PackedSemiPlanar: return "OMX_COLOR_FormatYUV422PackedSemiPlanar: Planes fragmented when a frame is split in multiple buffers";
        case OMX_COLOR_Format18BitBGR666:            return "OMX_COLOR_Format18BitBGR666";
        case OMX_COLOR_Format24BitARGB6666:          return "OMX_COLOR_Format24BitARGB6666";
        case OMX_COLOR_Format24BitABGR6666:          return "OMX_COLOR_Format24BitABGR6666";
        case OMX_COLOR_Format32bitABGR8888:          return "OMX_COLOR_Format32bitABGR8888";
        case OMX_COLOR_Format8bitPalette:            return "OMX_COLOR_Format8bitPalette";
        case OMX_COLOR_FormatYUVUV128:               return "OMX_COLOR_FormatYUVUV128";
        case OMX_COLOR_FormatRawBayer12bit:          return "OMX_COLOR_FormatRawBayer12bit";
        case OMX_COLOR_FormatBRCMEGL:                return "OMX_COLOR_FormatBRCMEGL";
        case OMX_COLOR_FormatBRCMOpaque:             return "OMX_COLOR_FormatBRCMOpaque";
        case OMX_COLOR_FormatYVU420PackedPlanar:     return "OMX_COLOR_FormatYVU420PackedPlanar";
        case OMX_COLOR_FormatYVU420PackedSemiPlanar: return "OMX_COLOR_FormatYVU420PackedSemiPlanar";
        default:
            f = calloc(23, sizeof(char));
            if(f == NULL) {
                die("Failed to allocate memory");
            }
            snprintf(f, 23 * sizeof(char) - 1, "format type 0x%08x", c);
            return f;
    }
}

static void dump_portdef(OMX_PARAM_PORTDEFINITIONTYPE* portdef) {
    say("Port %d is %s, %s, buffers wants:%d needs:%d, size:%d, pop:%d, aligned:%d",
        portdef->nPortIndex,
        (portdef->eDir ==  OMX_DirInput ? "input" : "output"),
        (portdef->bEnabled == OMX_TRUE ? "enabled" : "disabled"),
        portdef->nBufferCountActual,
        portdef->nBufferCountMin,
        portdef->nBufferSize,
        portdef->bPopulated,
        portdef->nBufferAlignment);

    OMX_VIDEO_PORTDEFINITIONTYPE *viddef = &portdef->format.video;
    OMX_IMAGE_PORTDEFINITIONTYPE *imgdef = &portdef->format.image;
    switch(portdef->eDomain) {
        case OMX_PortDomainVideo:
            say("Video type:\n"
                "\tWidth:\t\t%d\n"
                "\tHeight:\t\t%d\n"
                "\tStride:\t\t%d\n"
                "\tSliceHeight:\t%d\n"
                "\tBitrate:\t%d\n"
                "\tFramerate:\t%.02f\n"
                "\tError hiding:\t%s\n"
                "\tCodec:\t\t%s\n"
                "\tColor:\t\t%s\n",
                viddef->nFrameWidth,
                viddef->nFrameHeight,
                viddef->nStride,
                viddef->nSliceHeight,
                viddef->nBitrate,
                ((float)viddef->xFramerate / (float)65536),
                (viddef->bFlagErrorConcealment == OMX_TRUE ? "yes" : "no"),
                dump_compression_format(viddef->eCompressionFormat),
                dump_color_format(viddef->eColorFormat));
            break;
        case OMX_PortDomainImage:
            say("Image type:\n"
                "\tWidth:\t\t%d\n"
                "\tHeight:\t\t%d\n"
                "\tStride:\t\t%d\n"
                "\tSliceHeight:\t%d\n"
                "\tError hiding:\t%s\n"
                "\tCodec:\t\t%s\n"
                "\tColor:\t\t%s\n",
                imgdef->nFrameWidth,
                imgdef->nFrameHeight,
                imgdef->nStride,
                imgdef->nSliceHeight,
                (imgdef->bFlagErrorConcealment == OMX_TRUE ? "yes" : "no"),
                dump_compression_format(imgdef->eCompressionFormat),
                dump_color_format(imgdef->eColorFormat));
            break;
        default:
            break;
    }
}

static void dump_port(OMX_HANDLETYPE hComponent, OMX_U32 nPortIndex, OMX_BOOL dumpformats) {
    OMX_ERRORTYPE r;
    OMX_PARAM_PORTDEFINITIONTYPE portdef;
    OMX_INIT_STRUCTURE(portdef);
    portdef.nPortIndex = nPortIndex;
    if((r = OMX_GetParameter(hComponent, OMX_IndexParamPortDefinition, &portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get port definition for port %d", nPortIndex);
    }
    dump_portdef(&portdef);
    if(dumpformats) {
        OMX_VIDEO_PARAM_PORTFORMATTYPE portformat;
        OMX_INIT_STRUCTURE(portformat);
        portformat.nPortIndex = nPortIndex;
        portformat.nIndex = 0;
        r = OMX_ErrorNone;
        say("Port %d supports these video formats:", nPortIndex);
        while(r == OMX_ErrorNone) {
        if((r = OMX_GetParameter(hComponent, OMX_IndexParamVideoPortFormat, &portformat)) == OMX_ErrorNone) {
                say("\t%s, compression: %s", dump_color_format(portformat.eColorFormat), dump_compression_format(portformat.eCompressionFormat));
                portformat.nIndex++;
            }
        }
    }
}

// Some busy loops to verify we're running in order
static void block_until_state_changed(OMX_HANDLETYPE hComponent, OMX_STATETYPE wanted_eState) {
    OMX_STATETYPE eState;
    int i = 0;
    while(i++ == 0 || eState != wanted_eState) {
        OMX_GetState(hComponent, &eState);
        if(eState != wanted_eState) {
            usleep(10000);
        }
    }
}

static void block_until_port_changed(OMX_HANDLETYPE hComponent, OMX_U32 nPortIndex, OMX_BOOL bEnabled) {
    OMX_ERRORTYPE r;
    OMX_PARAM_PORTDEFINITIONTYPE portdef;
    OMX_INIT_STRUCTURE(portdef);
    portdef.nPortIndex = nPortIndex;
    OMX_U32 i = 0;
    while(i++ == 0 || portdef.bEnabled != bEnabled) {
        if((r = OMX_GetParameter(hComponent, OMX_IndexParamPortDefinition, &portdef)) != OMX_ErrorNone) {
            omx_die(r, "Failed to get port definition");
        }
        if(portdef.bEnabled != bEnabled) {
            usleep(10000);
        }
    }
}

static void block_until_flushed(appctx *ctx) {
    int quit;
    while(!quit) {
        vcos_semaphore_wait(&ctx->handler_lock);
        if(ctx->flushed) {
            ctx->flushed = 0;
            quit = 1;
        }
        vcos_semaphore_post(&ctx->handler_lock);
        if(!quit) {
            usleep(10000);
        }
    }
}

/*------------------------------------------------

  get handler for the component
  (?? it create an instance ??)

--------------------------------------------------*/

static void init_component_handle(
        const char *name,
        OMX_HANDLETYPE* hComponent,
        OMX_PTR pAppData,
        OMX_CALLBACKTYPE* callbacks) {
    OMX_ERRORTYPE r;
    char fullname[64]; // 32 is too short for splitter 

    // Get handle
    memset(fullname, 0, sizeof(fullname));
    strcat(fullname, "OMX.broadcom.");
    //strncat(fullname, name, strlen(fullname) - 1); // @TODO check why it cut the length
    strcat(fullname, name);
    say("Initializing component %s", fullname);
    //say(fullname);
    if((r = OMX_GetHandle(hComponent, fullname, pAppData, callbacks)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get handle for component %s", fullname);
    }

    // 2. Disable ports
    // since we donot know the details of component here, just disable all the ports after succeeding in query it. 
    OMX_INDEXTYPE types[] = {
        OMX_IndexParamAudioInit,
        OMX_IndexParamVideoInit,
        OMX_IndexParamImageInit,
        OMX_IndexParamOtherInit
    };
    OMX_PORT_PARAM_TYPE ports;
    OMX_INIT_STRUCTURE(ports);
    OMX_GetParameter(*hComponent, OMX_IndexParamVideoInit, &ports);

    int i;
    for(i = 0; i < 4; i++) {
        if(OMX_GetParameter(*hComponent, types[i], &ports) == OMX_ErrorNone) {
            OMX_U32 nPortIndex;
            for(nPortIndex = ports.nStartPortNumber; nPortIndex < ports.nStartPortNumber + ports.nPorts; nPortIndex++) {
                say("Disabling port %d of component %s", nPortIndex, fullname);
                if((r = OMX_SendCommand(*hComponent, OMX_CommandPortDisable, nPortIndex, NULL)) != OMX_ErrorNone) {
                    omx_die(r, "Failed to disable port %d of component %s", nPortIndex, fullname);
                }
                block_until_port_changed(*hComponent, nPortIndex, OMX_FALSE);
            }
        }
    }
}

// Global signal handler for trapping SIGINT, SIGTERM, and SIGQUIT
static void signal_handler(int signal) {
    want_quit = 1;
}

// OMX calls this handler for all the events it emits
static OMX_ERRORTYPE event_handler(
        OMX_HANDLETYPE hComponent,
        OMX_PTR pAppData,
        OMX_EVENTTYPE eEvent,
        OMX_U32 nData1,
        OMX_U32 nData2,
        OMX_PTR pEventData) {

    dump_event(hComponent, eEvent, nData1, nData2);

    appctx *ctx = (appctx *)pAppData;

    switch(eEvent) {
        case OMX_EventCmdComplete:
            vcos_semaphore_wait(&ctx->handler_lock);
            if(nData1 == OMX_CommandFlush) {
                ctx->flushed = 1;
            }
            vcos_semaphore_post(&ctx->handler_lock);
            break;
        case OMX_EventParamOrConfigChanged:
            vcos_semaphore_wait(&ctx->handler_lock);
            if(nData2 == OMX_IndexParamCameraDeviceNumber) {
                ctx->camera_ready = 1;
            }
            vcos_semaphore_post(&ctx->handler_lock);
            break;
        case OMX_EventError:
            omx_die(nData1, "error event received");
            break;
        default:
            break;
    }

    return OMX_ErrorNone;
}

// Called by OMX when the encoder component has filled
// the output buffer with H.264 encoded video data
static OMX_ERRORTYPE fill_output_buffer_done_handler(
        OMX_HANDLETYPE hComponent,
        OMX_PTR pAppData,
        OMX_BUFFERHEADERTYPE* pBuffer) {
    appctx *ctx = ((appctx*)pAppData);
    vcos_semaphore_wait(&ctx->handler_lock);
    // The main loop can now flush the buffer to output file
    if(hComponent == ctx->encoder)
    		ctx->encoder_output_buffer_available = 1;
#ifndef ORIGNAL
    else if(hComponent == ctx->encoder2)
    		ctx->encoder_output_buffer_available2 = 1;
#endif


    vcos_semaphore_post(&ctx->handler_lock);
    return OMX_ErrorNone;
}


/*-------------------------------------------------------------------- 
    input  : 73  
    output : 71, 70
--------------------------------------------------------------------*/ 
static int rpiomx_camera_setup(appctx *pctx, OMX_PARAM_PORTDEFINITIONTYPE *pcamera_portdef)
{
    OMX_ERRORTYPE r;

    say("Default port definition for camera input port 73");
    dump_port(pctx->camera, 73, OMX_TRUE);
    say("Default port definition for camera preview output port 70");
    dump_port(pctx->camera, 70, OMX_TRUE);
    say("Default port definition for camera video output port 71");
    dump_port(pctx->camera, 71, OMX_TRUE);
	
    // Request a callback to be made when OMX_IndexParamCameraDeviceNumber is
    // changed signaling that the camera device is ready for use.
    OMX_CONFIG_REQUESTCALLBACKTYPE cbtype;
    OMX_INIT_STRUCTURE(cbtype);
    cbtype.nPortIndex = OMX_ALL;
    cbtype.nIndex     = OMX_IndexParamCameraDeviceNumber;
    cbtype.bEnable    = OMX_TRUE;
    if((r = OMX_SetConfig(pctx->camera, OMX_IndexConfigRequestCallback, &cbtype)) != OMX_ErrorNone) {
        omx_die(r, "Failed to request camera device number parameter change callback for camera");
    }
    // Set device number, this triggers the callback configured just above
    OMX_PARAM_U32TYPE device;
    OMX_INIT_STRUCTURE(device);
    device.nPortIndex = OMX_ALL;
    device.nU32 = CAM_DEVICE_NUMBER;
    if((r = OMX_SetParameter(pctx->camera, OMX_IndexParamCameraDeviceNumber, &device)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera parameter device number");
    }


    // Configure video format emitted by camera preview output port
    //OMX_PARAM_PORTDEFINITIONTYPE camera_portdef;
    OMX_INIT_STRUCTURE(*pcamera_portdef);
    pcamera_portdef->nPortIndex = 70;
    if((r = OMX_GetParameter(pctx->camera, OMX_IndexParamPortDefinition, pcamera_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get port definition for camera preview output port 70");
    }
    pcamera_portdef->format.video.nFrameWidth  = VIDEO_WIDTH;
    pcamera_portdef->format.video.nFrameHeight = VIDEO_HEIGHT;
    pcamera_portdef->format.video.xFramerate   = VIDEO_FRAMERATE << 16;
    // Stolen from gstomxvideodec.c of gst-omx
    pcamera_portdef->format.video.nStride      = (pcamera_portdef->format.video.nFrameWidth + pcamera_portdef->nBufferAlignment - 1) & (~(pcamera_portdef->nBufferAlignment - 1));
    pcamera_portdef->format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
    if((r = OMX_SetParameter(pctx->camera, OMX_IndexParamPortDefinition, pcamera_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set port definition for camera preview output port 70");
    }
    // Configure video format emitted by camera video output port
    // Use configuration from camera preview output as basis for
    // camera video output configuration
    OMX_INIT_STRUCTURE(*pcamera_portdef);
    pcamera_portdef->nPortIndex = 70;
    if((r = OMX_GetParameter(pctx->camera, OMX_IndexParamPortDefinition, pcamera_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get port definition for camera preview output port 70");
    }
    pcamera_portdef->nPortIndex = 71;
    if((r = OMX_SetParameter(pctx->camera, OMX_IndexParamPortDefinition, pcamera_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set port definition for camera video output port 71");
    }
    // Configure frame rate
    OMX_CONFIG_FRAMERATETYPE framerate;
    OMX_INIT_STRUCTURE(framerate);
    framerate.nPortIndex = 70;
    framerate.xEncodeFramerate = pcamera_portdef->format.video.xFramerate;
    if((r = OMX_SetConfig(pctx->camera, OMX_IndexConfigVideoFramerate, &framerate)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set framerate configuration for camera preview output port 70");
    }
    framerate.nPortIndex = 71;
    if((r = OMX_SetConfig(pctx->camera, OMX_IndexConfigVideoFramerate, &framerate)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set framerate configuration for camera video output port 71");
    }
    // Configure sharpness
    OMX_CONFIG_SHARPNESSTYPE sharpness;
    OMX_INIT_STRUCTURE(sharpness);
    sharpness.nPortIndex = OMX_ALL;
    sharpness.nSharpness = CAM_SHARPNESS;
    if((r = OMX_SetConfig(pctx->camera, OMX_IndexConfigCommonSharpness, &sharpness)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera sharpness configuration");
    }
    // Configure contrast
    OMX_CONFIG_CONTRASTTYPE contrast;
    OMX_INIT_STRUCTURE(contrast);
    contrast.nPortIndex = OMX_ALL;
    contrast.nContrast = CAM_CONTRAST;
    if((r = OMX_SetConfig(pctx->camera, OMX_IndexConfigCommonContrast, &contrast)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera contrast configuration");
    }
    // Configure saturation
    OMX_CONFIG_SATURATIONTYPE saturation;
    OMX_INIT_STRUCTURE(saturation);
    saturation.nPortIndex = OMX_ALL;
    saturation.nSaturation = CAM_SATURATION;
    if((r = OMX_SetConfig(pctx->camera, OMX_IndexConfigCommonSaturation, &saturation)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera saturation configuration");
    }
    // Configure brightness
    OMX_CONFIG_BRIGHTNESSTYPE brightness;
    OMX_INIT_STRUCTURE(brightness);
    brightness.nPortIndex = OMX_ALL;
    brightness.nBrightness = CAM_BRIGHTNESS;
    if((r = OMX_SetConfig(pctx->camera, OMX_IndexConfigCommonBrightness, &brightness)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera brightness configuration");
    }
    // Configure exposure value
    OMX_CONFIG_EXPOSUREVALUETYPE exposure_value;
    OMX_INIT_STRUCTURE(exposure_value);
    exposure_value.nPortIndex = OMX_ALL;
    exposure_value.xEVCompensation = CAM_EXPOSURE_VALUE_COMPENSTAION;
    exposure_value.bAutoSensitivity = CAM_EXPOSURE_AUTO_SENSITIVITY;
    exposure_value.nSensitivity = CAM_EXPOSURE_ISO_SENSITIVITY;
    if((r = OMX_SetConfig(pctx->camera, OMX_IndexConfigCommonExposureValue, &exposure_value)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera exposure value configuration");
    }
    // Configure frame frame stabilisation
    OMX_CONFIG_FRAMESTABTYPE frame_stabilisation_control;
    OMX_INIT_STRUCTURE(frame_stabilisation_control);
    frame_stabilisation_control.nPortIndex = OMX_ALL;
    frame_stabilisation_control.bStab = CAM_FRAME_STABILISATION;
    if((r = OMX_SetConfig(pctx->camera, OMX_IndexConfigCommonFrameStabilisation, &frame_stabilisation_control)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera frame frame stabilisation control configuration");
    }
    // Configure frame white balance control
    OMX_CONFIG_WHITEBALCONTROLTYPE white_balance_control;
    OMX_INIT_STRUCTURE(white_balance_control);
    white_balance_control.nPortIndex = OMX_ALL;
    white_balance_control.eWhiteBalControl = CAM_WHITE_BALANCE_CONTROL;
    if((r = OMX_SetConfig(pctx->camera, OMX_IndexConfigCommonWhiteBalance, &white_balance_control)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera frame white balance control configuration");
    }
    // Configure image filter
    OMX_CONFIG_IMAGEFILTERTYPE image_filter;
    OMX_INIT_STRUCTURE(image_filter);
    image_filter.nPortIndex = OMX_ALL;
    image_filter.eImageFilter = CAM_IMAGE_FILTER;
    if((r = OMX_SetConfig(pctx->camera, OMX_IndexConfigCommonImageFilter, &image_filter)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera image filter configuration");
    }
    // Configure mirror
    OMX_MIRRORTYPE eMirror = OMX_MirrorNone;
    if(CAM_FLIP_HORIZONTAL && !CAM_FLIP_VERTICAL) {
        eMirror = OMX_MirrorHorizontal;
    } else if(!CAM_FLIP_HORIZONTAL && CAM_FLIP_VERTICAL) {
        eMirror = OMX_MirrorVertical;
    } else if(CAM_FLIP_HORIZONTAL && CAM_FLIP_VERTICAL) {
        eMirror = OMX_MirrorBoth;
    }
    OMX_CONFIG_MIRRORTYPE mirror;
    OMX_INIT_STRUCTURE(mirror);
    mirror.nPortIndex = 71;
    mirror.eMirror = eMirror;
    if((r = OMX_SetConfig(pctx->camera, OMX_IndexConfigCommonMirror, &mirror)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set mirror configuration for camera video output port 71");
    }
    return 0;
}

#ifndef ORIGINAL
/*------------------------------------------------
  video_splitter component setup using camera output port format 


-------------------------------------------------*/
static int rpiomx_video_splitter_setup(appctx *pctx, OMX_PARAM_PORTDEFINITIONTYPE *pcamera_portdef)
{
    //OMX_ERRORTYPE r;
    say("Default port definition for video splitter input port 250");
    dump_port(pctx->video_splitter, 250, OMX_TRUE);
    say("Default port definition for video splitter output port 251");
    dump_port(pctx->video_splitter, 251, OMX_TRUE);
    say("Default port definition for video splitter output port 252");
    dump_port(pctx->video_splitter, 252, OMX_TRUE);
    say("Default port definition for video splitter output port 253");
    dump_port(pctx->video_splitter, 253, OMX_TRUE);
    say("Default port definition for video splitter output port 254");
    dump_port(pctx->video_splitter, 254, OMX_TRUE);

    // video splitter input port definition is done automatically upon tunneling?????

    // how about the output ??

    return 0;
}
#endif 

/*------------------------------------------------
  encoder component setup using camera output port format 


-------------------------------------------------*/
static int rpiomx_encoder_setup(OMX_HANDLETYPE hcomp, OMX_PARAM_PORTDEFINITIONTYPE *pcamera_portdef)
{
    OMX_ERRORTYPE r;
    say("Default port definition for encoder input port 200");
    dump_port(hcomp, 200, OMX_TRUE);
    say("Default port definition for encoder output port 201");
    dump_port(hcomp, 201, OMX_TRUE);

    // Encoder input port definition is done automatically upon tunneling

    // Configure video format emitted by encoder output port
    OMX_PARAM_PORTDEFINITIONTYPE encoder_portdef;
    OMX_INIT_STRUCTURE(encoder_portdef);
    encoder_portdef.nPortIndex = 201;
    if((r = OMX_GetParameter(hcomp, OMX_IndexParamPortDefinition, &encoder_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get port definition for encoder output port 201");
    }
    // Copy some of the encoder output port configuration
    // from camera output port
    encoder_portdef.format.video.nFrameWidth  = pcamera_portdef->format.video.nFrameWidth;
    encoder_portdef.format.video.nFrameHeight = pcamera_portdef->format.video.nFrameHeight;
    encoder_portdef.format.video.xFramerate   = pcamera_portdef->format.video.xFramerate;
    encoder_portdef.format.video.nStride      = pcamera_portdef->format.video.nStride;
    // Which one is effective, this or the configuration just below?
    encoder_portdef.format.video.nBitrate     = VIDEO_BITRATE;
    if((r = OMX_SetParameter(hcomp, OMX_IndexParamPortDefinition, &encoder_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set port definition for encoder output port 201");
    }
    // Configure bitrate
    OMX_VIDEO_PARAM_BITRATETYPE bitrate;
    OMX_INIT_STRUCTURE(bitrate);
    bitrate.eControlRate = OMX_Video_ControlRateVariable;
    bitrate.nTargetBitrate = encoder_portdef.format.video.nBitrate;
    bitrate.nPortIndex = 201;
    if((r = OMX_SetParameter(hcomp, OMX_IndexParamVideoBitrate, &bitrate)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set bitrate for encoder output port 201");
    }
    // Configure format
    OMX_VIDEO_PARAM_PORTFORMATTYPE format;
    OMX_INIT_STRUCTURE(format);
    format.nPortIndex = 201;
    format.eCompressionFormat = OMX_VIDEO_CodingAVC;
    if((r = OMX_SetParameter(hcomp, OMX_IndexParamVideoPortFormat, &format)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set video format for encoder output port 201");
    }

    return 0;
}

/*---------------------------------------------

  1. set all components into idle state from loaded
  2. enable all ports necessary  
  3. allocate all non-tunnelng buffer
-------------------------------------------------*/
static int rpiomx_graph_ready(appctx *pctx)
{
    OMX_ERRORTYPE r;

    // 1. Switch components to idle state
    say("Switching state of the camera component to idle...");
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the camera component to idle");
    }
    block_until_state_changed(pctx->camera, OMX_StateIdle);
#ifndef ORIGINAL
    say("Switching state of the video_splitter component to idle...");
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the video_splitter component to idle");
    }
    block_until_state_changed(pctx->video_splitter, OMX_StateIdle);
    say("Switching state of the encoder2 component to idle...");
    if((r = OMX_SendCommand(pctx->encoder2, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the encoder2 component to idle");
    }
    block_until_state_changed(pctx->encoder2, OMX_StateIdle);
#endif
    say("Switching state of the encoder component to idle...");
    if((r = OMX_SendCommand(pctx->encoder, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the encoder component to idle");
    }
    block_until_state_changed(pctx->encoder, OMX_StateIdle);
    say("Switching state of the null sink component to idle...");
    if((r = OMX_SendCommand(pctx->null_sink, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the null sink component to idle");
    }
    block_until_state_changed(pctx->null_sink, OMX_StateIdle);
#ifndef ORIGINAL
    say("Switching state of the write_media component to idle...");
    if((r = OMX_SendCommand(pctx->write_media, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the write_media component to idle");
    }
    block_until_state_changed(pctx->write_media, OMX_StateIdle);
#endif

    // 2.  Enable ports
    say("Enabling ports...");
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandPortEnable, 73, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable camera input port 73");
    }
    block_until_port_changed(pctx->camera, 73, OMX_TRUE);
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandPortEnable, 70, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable camera preview output port 70");
    }
    block_until_port_changed(pctx->camera, 70, OMX_TRUE);
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandPortEnable, 71, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable camera video output port 71");
    }
    block_until_port_changed(pctx->camera, 71, OMX_TRUE);

#ifndef ORIGINAL
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandPortEnable, 250, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable video_splitter input  port 250");
    }
    block_until_port_changed(pctx->video_splitter, 250, OMX_TRUE);
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandPortEnable, 251, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable video_splitter input  port 251");
    }
    block_until_port_changed(pctx->video_splitter, 251, OMX_TRUE);
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandPortEnable, 252, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable video_splitter input  port 252");
    }
    block_until_port_changed(pctx->video_splitter, 251, OMX_TRUE);

    if((r = OMX_SendCommand(pctx->encoder2, OMX_CommandPortEnable, 200, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable encoder input port 200");
    }
    block_until_port_changed(pctx->encoder2, 200, OMX_TRUE);
    if((r = OMX_SendCommand(pctx->encoder2, OMX_CommandPortEnable, 201, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable encoder output port 201");
    }
    block_until_port_changed(pctx->encoder2, 201, OMX_TRUE);
#endif


    if((r = OMX_SendCommand(pctx->encoder, OMX_CommandPortEnable, 200, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable encoder input port 200");
    }
    block_until_port_changed(pctx->encoder, 200, OMX_TRUE);
    if((r = OMX_SendCommand(pctx->encoder, OMX_CommandPortEnable, 201, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable encoder output port 201");
    }
    block_until_port_changed(pctx->encoder, 201, OMX_TRUE);
    if((r = OMX_SendCommand(pctx->null_sink, OMX_CommandPortEnable, 240, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable null sink input port 240");
    }
    block_until_port_changed(pctx->null_sink, 240, OMX_TRUE);

    // 3. Allocate camera input buffer and encoder output buffer,
    // buffers for tunneled ports are allocated internally by OMX
    say("Allocating buffers...");
    OMX_PARAM_PORTDEFINITIONTYPE camera_portdef;
    OMX_INIT_STRUCTURE(camera_portdef);
    camera_portdef.nPortIndex = 73;
    if((r = OMX_GetParameter(pctx->camera, OMX_IndexParamPortDefinition, &camera_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get port definition for camera input port 73");
    }
    if((r = OMX_AllocateBuffer(pctx->camera, &pctx->camera_ppBuffer_in, 73, NULL, camera_portdef.nBufferSize)) != OMX_ErrorNone) {
        omx_die(r, "Failed to allocate buffer for camera input port 73");
    }

#ifndef ORIGINAL
    say("buffers for tunneled ports are allocated internally by OMX");
    OMX_PARAM_PORTDEFINITIONTYPE encoder_portdef;
    OMX_INIT_STRUCTURE(encoder_portdef);
    encoder_portdef.nPortIndex = 201;
    if((r = OMX_GetParameter(pctx->encoder2, OMX_IndexParamPortDefinition, &encoder_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get port definition for encoder2 output port 201");
    }
    if((r = OMX_AllocateBuffer(pctx->encoder2, &pctx->encoder_ppBuffer_out2, 201, NULL, encoder_portdef.nBufferSize)) != OMX_ErrorNone) {
        omx_die(r, "Failed to allocate buffer for encoder 2 output port 201");
    }
#endif

    //OMX_PARAM_PORTDEFINITIONTYPE encoder_portdef;
    OMX_INIT_STRUCTURE(encoder_portdef);
    encoder_portdef.nPortIndex = 201;
    if((r = OMX_GetParameter(pctx->encoder, OMX_IndexParamPortDefinition, &encoder_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get port definition for encoder output port 201");
    }
    if((r = OMX_AllocateBuffer(pctx->encoder, &pctx->encoder_ppBuffer_out, 201, NULL, encoder_portdef.nBufferSize)) != OMX_ErrorNone) {
        omx_die(r, "Failed to allocate buffer for encoder output port 201");
    }

    return 0;
}

/*----------------------------------------

  Running 

  1. turn components into executing state
  2. in the order of camera => encoder & sink
  then turn on the output port (why here??)

-----------------------------------------*/
static int rpiomx_graph_fire(appctx *pctx)
{
    OMX_ERRORTYPE r;
    // Switch state of the components prior to starting
    // the video capture and encoding loop
    say("Switching state of the camera component to executing...");
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandStateSet, OMX_StateExecuting, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the camera component to executing");
    }
    block_until_state_changed(pctx->camera, OMX_StateExecuting);

#ifndef ORIGINAL
    say("Switching state of the video_splitter component to executing...");
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandStateSet, OMX_StateExecuting, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the video_splitter component to executing");
    }
    block_until_state_changed(pctx->video_splitter, OMX_StateExecuting);

    say("Switching state of the encoder 2 component to executing...");
    if((r = OMX_SendCommand(pctx->encoder2, OMX_CommandStateSet, OMX_StateExecuting, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the encoder component to executing");
    }
    block_until_state_changed(pctx->encoder2, OMX_StateExecuting);

#endif

    say("Switching state of the encoder component to executing...");
    if((r = OMX_SendCommand(pctx->encoder, OMX_CommandStateSet, OMX_StateExecuting, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the encoder component to executing");
    }
    block_until_state_changed(pctx->encoder, OMX_StateExecuting);
    say("Switching state of the null sink component to executing...");
    if((r = OMX_SendCommand(pctx->null_sink, OMX_CommandStateSet, OMX_StateExecuting, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the null sink component to executing");
    }
    block_until_state_changed(pctx->null_sink, OMX_StateExecuting);

    // 2. Start capturing video with the camera
    say("Switching on capture on camera video output port 71...");
    OMX_CONFIG_PORTBOOLEANTYPE capture;
    OMX_INIT_STRUCTURE(capture);
    capture.nPortIndex = 71;
    capture.bEnabled = OMX_TRUE;
    if((r = OMX_SetParameter(pctx->camera, OMX_IndexConfigPortCapturing, &capture)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch on capture on camera video output port 71");
    }

    return 0;
}


/*-----------------------------------
   stop the running ghraph 

   1. stop output port 71
   2. then flush all the remaing data in buffers

   still active graph but not running 
------------------------------------------*/
static int rpiomx_graph_shutdown(appctx *pctx)
{
    OMX_ERRORTYPE r;

    // 1. stop capturing 
    OMX_CONFIG_PORTBOOLEANTYPE capture;
    OMX_INIT_STRUCTURE(capture);
    capture.nPortIndex = 71;
    capture.bEnabled = OMX_FALSE;
    if((r = OMX_SetParameter(pctx->camera, OMX_IndexConfigPortCapturing, &capture)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch off capture on camera video output port 71");
    }

    // ???
    // 2. Return the last full buffer back to the encoder component
    pctx->encoder_ppBuffer_out->nFlags = OMX_BUFFERFLAG_EOS;
    if((r = OMX_FillThisBuffer(pctx->encoder, pctx->encoder_ppBuffer_out)) != OMX_ErrorNone) {
        omx_die(r, "Failed to request filling of the output buffer on encoder output port 201");
    }
#ifndef ORIGINAL
    pctx->encoder_ppBuffer_out2->nFlags = OMX_BUFFERFLAG_EOS;
    if((r = OMX_FillThisBuffer(pctx->encoder2, pctx->encoder_ppBuffer_out2)) != OMX_ErrorNone) {
        omx_die(r, "Failed to request filling of the output buffer on encoder output port 201");
    }
#endif

    // 3.Flush the buffers on each component
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandFlush, 73, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of camera input port 73");
    }
    block_until_flushed(pctx);
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandFlush, 70, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of camera preview output port 70");
    }
    block_until_flushed(pctx);
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandFlush, 71, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of camera video output port 71");
    }
    block_until_flushed(pctx);

#ifndef ORIGINAL
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandFlush, 250, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of video_splitter input port 250");
    }
    block_until_flushed(pctx);
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandFlush, 251, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of video_splitter output port 251");
    }
    block_until_flushed(pctx);
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandFlush, 252, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of video_splitter output port 251");
    }
    block_until_flushed(pctx);

    if((r = OMX_SendCommand(pctx->encoder2, OMX_CommandFlush, 200, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of encoder input port 200");
    }
    block_until_flushed(pctx);
#endif


    if((r = OMX_SendCommand(pctx->encoder, OMX_CommandFlush, 200, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of encoder input port 200");
    }
    block_until_flushed(pctx);
    if((r = OMX_SendCommand(pctx->encoder, OMX_CommandFlush, 201, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of encoder output port 201");
    }
    block_until_flushed(pctx);
    if((r = OMX_SendCommand(pctx->null_sink, OMX_CommandFlush, 240, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of null sink input port 240");
    }
    block_until_flushed(pctx);

    return 0;
}


#if 0 
/*----------------------------------------------
  port enable or disabled
-----------------------------------------------*/
static int rpiomx_port_disable(OMX_HANDLETYPE hcomp, int portnum, const char *portname)
{
    if((r = OMX_SendCommand(hcomp, OMX_CommandPortDisable, portnum, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable %s %d", portname, portnum);
    }
    block_until_port_changed(hcomp, portnum, OMX_FALSE);

    return 0;
}
#endif


/*------------------------------------------------

   1. disables ports of camera, and encoders
   2. free buffers
   3. put the components into idle state  
   4. put the components into loaded state  

-------------------------------------------------*/
static int rpiomx_graph_teardown(appctx *pctx)
{

    OMX_ERRORTYPE r;

    // 1. Disable all the ports
    // camera ports
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandPortDisable, 73, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable camera input port 73");
    }
    block_until_port_changed(pctx->camera, 73, OMX_FALSE);
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandPortDisable, 70, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable camera preview output port 70");
    }
    block_until_port_changed(pctx->camera, 70, OMX_FALSE);
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandPortDisable, 71, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable camera video output port 71");
    }
    block_until_port_changed(pctx->camera, 71, OMX_FALSE);


#ifndef ORIGINAL
    // video_splitter ports
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandPortDisable, 250, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable video_splitter input port 250");
    }
    block_until_port_changed(pctx->video_splitter, 250, OMX_FALSE);
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandPortDisable, 251, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable video_splitter output port 251");
    }
    block_until_port_changed(pctx->video_splitter, 251, OMX_FALSE);
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandPortDisable, 252, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable video_splitter output port 252");
    }
    block_until_port_changed(pctx->video_splitter, 252, OMX_FALSE);

    if((r = OMX_SendCommand(pctx->encoder2, OMX_CommandPortDisable, 200, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable encoder input port 200");
    }
    block_until_port_changed(pctx->encoder2, 200, OMX_FALSE);
    if((r = OMX_SendCommand(pctx->encoder2, OMX_CommandPortDisable, 201, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable encoder output port 201");
    }
    block_until_port_changed(pctx->encoder2, 201, OMX_FALSE);
#endif

    // encoder ports
    if((r = OMX_SendCommand(pctx->encoder, OMX_CommandPortDisable, 200, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable encoder input port 200");
    }
    block_until_port_changed(pctx->encoder, 200, OMX_FALSE);
    if((r = OMX_SendCommand(pctx->encoder, OMX_CommandPortDisable, 201, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable encoder output port 201");
    }
    block_until_port_changed(pctx->encoder, 201, OMX_FALSE);


    if((r = OMX_SendCommand(pctx->null_sink, OMX_CommandPortDisable, 240, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable null sink input port 240");
    }
    block_until_port_changed(pctx->null_sink, 240, OMX_FALSE);

    // 2. Free all the buffers
    if((r = OMX_FreeBuffer(pctx->camera, 73, pctx->camera_ppBuffer_in)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free buffer for camera input port 73");
    }
#ifndef ORIGINAL
    if((r = OMX_FreeBuffer(pctx->encoder2, 201, pctx->encoder_ppBuffer_out2)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free buffer for encoder output port 201");
    }
#endif
    if((r = OMX_FreeBuffer(pctx->encoder, 201, pctx->encoder_ppBuffer_out)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free buffer for encoder output port 201");
    }

    // 3. Transition all the components to idle and then to loaded states
    // to idle state
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the camera component to idle");
    }
    block_until_state_changed(pctx->camera, OMX_StateIdle);
#ifndef ORIGINAL
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the video_splitter component to idle");
    }
    block_until_state_changed(pctx->video_splitter, OMX_StateIdle);
    if((r = OMX_SendCommand(pctx->encoder2, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the encoder 2 component to idle");
    }
    block_until_state_changed(pctx->encoder2, OMX_StateIdle);
#endif
    if((r = OMX_SendCommand(pctx->encoder, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the encoder component to idle");
    }
    block_until_state_changed(pctx->encoder, OMX_StateIdle);
    if((r = OMX_SendCommand(pctx->null_sink, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the null sink component to idle");
    }
    block_until_state_changed(pctx->null_sink, OMX_StateIdle);


    // to loaded state
    if((r = OMX_SendCommand(pctx->camera, OMX_CommandStateSet, OMX_StateLoaded, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the camera component to loaded");
    }
    block_until_state_changed(pctx->camera, OMX_StateLoaded);

#ifndef ORIGINAL
    if((r = OMX_SendCommand(pctx->video_splitter, OMX_CommandStateSet, OMX_StateLoaded, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the video_splitter component to loaded");
    }
    block_until_state_changed(pctx->video_splitter, OMX_StateLoaded);
    if((r = OMX_SendCommand(pctx->encoder2, OMX_CommandStateSet, OMX_StateLoaded, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the encoder  2 component to loaded");
    }
    block_until_state_changed(pctx->encoder2, OMX_StateLoaded);
#endif

    if((r = OMX_SendCommand(pctx->encoder, OMX_CommandStateSet, OMX_StateLoaded, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the encoder component to loaded");
    }
    block_until_state_changed(pctx->encoder, OMX_StateLoaded);
    if((r = OMX_SendCommand(pctx->null_sink, OMX_CommandStateSet, OMX_StateLoaded, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the null sink component to loaded");
    }
    block_until_state_changed(pctx->null_sink, OMX_StateLoaded);

    return 0;

}

/*--------------------------------------------------------------------	/
/  main ()                                                               	/
/                                                                    	/ 
/ (73)camera(71) ---70							/	
/           (70)              
/
/
/  camera -- splitter -- encoder -- write_media
/                        resize  -- encoder 
/--------------------------------------------------------------------	*/
int main(int argc, char **argv) 
{
    OMX_ERRORTYPE r;

    // 0.1 system init
    bcm_host_init();


    // 0.2 init OMX manager
    if((r = OMX_Init()) != OMX_ErrorNone) {
        omx_die(r, "OMX initalization failed");
    }

    // 0.3 Init context
    appctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    if(vcos_semaphore_create(&ctx.handler_lock, "handler_lock", 1) != VCOS_SUCCESS) {
        die("Failed to create handler lock semaphore");
    }

    // 1.1 Init component handles
    OMX_CALLBACKTYPE callbacks;
    memset(&ctx, 0, sizeof(callbacks));
    callbacks.EventHandler   = event_handler;
    callbacks.FillBufferDone = fill_output_buffer_done_handler;

    init_component_handle("camera", &ctx.camera , &ctx, &callbacks);
#ifndef ORIGINAL
    init_component_handle("video_splitter", &ctx.video_splitter, &ctx, &callbacks);
    init_component_handle("resize", &ctx.resize, &ctx, &callbacks);
    init_component_handle("write_media", &ctx.write_media, &ctx, &callbacks);
    init_component_handle("video_encode", &ctx.encoder2, &ctx, &callbacks);
#endif
    init_component_handle("video_encode", &ctx.encoder, &ctx, &callbacks);
    init_component_handle("null_sink", &ctx.null_sink, &ctx, &callbacks);

    // 1.2.1 camera component setup 
    say("Configuring camera...");
    OMX_PARAM_PORTDEFINITIONTYPE camera_portdef;
    rpiomx_camera_setup(&ctx, &camera_portdef);

    // Ensure camera is ready
    int n = 0;
    while(!ctx.camera_ready) {
        usleep(10000);
	if(n++ > 100) die("Failed to configuring camera");
    }

#ifndef ORIGINAL
    say("Configuring video_splitter...");
    rpiomx_video_splitter_setup(&ctx, &camera_portdef);
#endif

    // 1.2.2 encoder compenent setup 
    say("Configuring encoder...");
    rpiomx_encoder_setup(ctx.encoder, &camera_portdef);
#ifndef ORIGINAL
    rpiomx_encoder_setup(ctx.encoder2, &camera_portdef);
#endif

    // 1.2.3 null sink setup
    // we may need one more null sink for splitter later 
    say("Configuring null sink...");
    say("Default port definition for null sink input port 240");
    dump_port(ctx.null_sink, 240, OMX_TRUE);

    // Null sink input port definition is done automatically upon tunneling

    // 1.3 build a graph using tunnelling
    // Tunnel camera preview output port and null sink input port
    say("Setting up tunnel from camera preview output port 70 to null sink input port 240...");
    if((r = OMX_SetupTunnel(ctx.camera, 70, ctx.null_sink, 240)) != OMX_ErrorNone) {
        omx_die(r, "Failed to setup tunnel between camera preview output port 70 and null sink input port 240");
    }

#ifdef ORIGINAL 
    // Tunnel camera video output port and encoder input port
    say("Setting up tunnel from camera video output port 71 to encoder input port 200...");
    if((r = OMX_SetupTunnel(ctx.camera, 71, ctx.encoder, 200)) != OMX_ErrorNone) {
        omx_die(r, "Failed to setup tunnel between camera video output port 71 and encoder input port 200");
    }
#else
    // Tunnel camera video output port and splitter input port
    say("Setting up tunnel from camera video output port 71 to splitter  input port 250...");
    if((r = OMX_SetupTunnel(ctx.camera, 71, ctx.video_splitter, 250)) != OMX_ErrorNone) {
        omx_die(r, "Failed to setup tunnel between camera video output port 71 and splitter input port 250");
    }
    // Tunnel camera video output port and encoder input port
    say("Setting up tunnel from splitter output port 251 to encoder input port 200...");
    if((r = OMX_SetupTunnel(ctx.video_splitter, 251, ctx.encoder, 200)) != OMX_ErrorNone) {
        omx_die(r, "Failed to setup tunnel between video_splitter output port 251 and encoder input port 200");
    }

    // Tunnel camera video output port and encoder input port
    say("Setting up tunnel from splitter output port 252 to encoder 2 input port 200...");
    if((r = OMX_SetupTunnel(ctx.video_splitter, 252, ctx.encoder2, 200)) != OMX_ErrorNone) {
        omx_die(r, "Failed to setup tunnel between video_splitter output port 252 and encoder2 input port 200");
    }
#endif


    // 1.4 get ready components 
    rpiomx_graph_ready(&ctx);

    // Just use stdout for output
    say("Opening output file...");
    ctx.fd_out = stdout;

    // 1.5 kick off the graph  
    rpiomx_graph_fire(&ctx);


    say("Configured port definition for camera input port 73");
    dump_port(ctx.camera, 73, OMX_FALSE);
    say("Configured port definition for camera preview output port 70");
    dump_port(ctx.camera, 70, OMX_FALSE);
    say("Configured port definition for camera video output port 71");
    dump_port(ctx.camera, 71, OMX_FALSE);
#ifndef ORIGINAL
    say("Configured port definition for video_splitter input port 250");
    dump_port(ctx.video_splitter, 250, OMX_FALSE);
    say("Configured port definition for video_splitter output port 251");
    dump_port(ctx.video_splitter, 251, OMX_FALSE);
    say("Configured port definition for video_splitter output port 252");
    dump_port(ctx.video_splitter, 252, OMX_FALSE);
    say("Configured port definition for encoder 2 input port 200");
    dump_port(ctx.encoder2, 200, OMX_FALSE);
    say("Configured port definition for encoder 2 output port 201");
    dump_port(ctx.encoder2, 201, OMX_FALSE);
#endif
    say("Configured port definition for encoder input port 200");
    dump_port(ctx.encoder, 200, OMX_FALSE);
    say("Configured port definition for encoder output port 201");
    dump_port(ctx.encoder, 201, OMX_FALSE);
    say("Configured port definition for null sink input port 240");
    dump_port(ctx.null_sink, 240, OMX_FALSE);


    say("Enter capture and encode loop, press Ctrl-C to quit...");

    int quit_detected = 0, quit_in_keyframe = 0, need_next_buffer_to_be_filled = 1;
    int quit_detected2 = 0, quit_in_keyframe2 = 0, need_next_buffer_to_be_filled2 = 1;
    size_t output_written;
    //size_t output_written2;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGQUIT, signal_handler);

    int nframe = 0;
    int nframe2 = 0;
    while(1) {


	// encoder # 1
        // fill_output_buffer_done_handler() has marked that there's
        // a buffer for us to flush
        if(ctx.encoder_output_buffer_available) {

            // Print a message if the user wants to quit, but don't exit
            // the loop until we are certain that we have processed
            // a full frame till end of the frame, i.e. we're at the end
            // of the current key frame if processing one or until
            // the next key frame is detected. This way we should always
            // avoid corruption of the last encoded at the expense of
            // small delay in exiting.
            if(want_quit && !quit_detected) {
                say("Exit signal detected, waiting for next key frame boundry before exiting...");
                quit_detected = 1;
                quit_in_keyframe = ctx.encoder_ppBuffer_out->nFlags & OMX_BUFFERFLAG_SYNCFRAME;
            }
            if(quit_detected && (quit_in_keyframe ^ (ctx.encoder_ppBuffer_out->nFlags & OMX_BUFFERFLAG_SYNCFRAME))) {
                say("Key frame boundry reached, exiting loop...");
                break;
            }
            // Flush buffer to output file
            output_written = fwrite(ctx.encoder_ppBuffer_out->pBuffer + ctx.encoder_ppBuffer_out->nOffset, 1, ctx.encoder_ppBuffer_out->nFilledLen, ctx.fd_out);
            if(output_written != ctx.encoder_ppBuffer_out->nFilledLen) {
                die("Failed to write to output file: %s", strerror(errno));
            }

            //say("Read from output buffer and wrote to output file %d/%d", ctx.encoder_ppBuffer_out->nFilledLen, ctx.encoder_ppBuffer_out->nAllocLen);
            say("fn=%d",++nframe);

		
            need_next_buffer_to_be_filled = 1;
        }
        // Buffer flushed, request a new buffer to be filled by the encoder component
        if(need_next_buffer_to_be_filled) {
            need_next_buffer_to_be_filled = 0;
            ctx.encoder_output_buffer_available = 0;
            if((r = OMX_FillThisBuffer(ctx.encoder, ctx.encoder_ppBuffer_out)) != OMX_ErrorNone) {
                omx_die(r, "Failed to request filling of the output buffer on encoder output port 201");
            }
        }

	// encoder # 2
        // fill_output_buffer_done_handler() has marked that there's
        // a buffer for us to flush
        if(ctx.encoder_output_buffer_available2) {

            // Print a message if the user wants to quit, but don't exit
            // the loop until we are certain that we have processed
            // a full frame till end of the frame, i.e. we're at the end
            // of the current key frame if processing one or until
            // the next key frame is detected. This way we should always
            // avoid corruption of the last encoded at the expense of
            // small delay in exiting.
            if(want_quit && !quit_detected2) {
                say("Exit signal detected, waiting for next key frame boundry before exiting...");
                quit_detected2 = 1;
                quit_in_keyframe2 = ctx.encoder_ppBuffer_out2->nFlags & OMX_BUFFERFLAG_SYNCFRAME;
            }
            if(quit_detected2 && (quit_in_keyframe2 ^ (ctx.encoder_ppBuffer_out2->nFlags & OMX_BUFFERFLAG_SYNCFRAME))) {
                say("Key frame boundry reached, exiting loop...");
                break;
            }
            // Flush buffer to output file
            // output_written = fwrite(ctx.encoder_ppBuffer_out2->pBuffer + ctx.encoder_ppBuffer_out2->nOffset, 1, ctx.encoder_ppBuffer_out2->nFilledLen, ctx.fd_out);
            //if(output_written != ctx.encoder_ppBuffer_out2->nFilledLen) {
            //    die("Failed to write to output file: %s", strerror(errno));
            //}

            //say("Read from output buffer and wrote to output file %d/%d", ctx.encoder_ppBuffer_out2->nFilledLen, ctx.encoder_ppBuffer_out2->nAllocLen);
            say("fn2=%d",++nframe2);
		
            need_next_buffer_to_be_filled2 = 1;
        }
        // Buffer flushed, request a new buffer to be filled by the encoder component
        if(need_next_buffer_to_be_filled2) {
            need_next_buffer_to_be_filled2 = 0;
            ctx.encoder_output_buffer_available2 = 0;
            if((r = OMX_FillThisBuffer(ctx.encoder2, ctx.encoder_ppBuffer_out2)) != OMX_ErrorNone) {
                omx_die(r, "Failed to request filling of the output buffer on encoder output port 201");
            }
        }


        // Would be better to use signaling here but hey this works too
        usleep(1000);
    }
    say("Cleaning up...");

    // Restore signal handlers
    signal(SIGINT,  SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);

    // 3.1 Stop capturing video with the camera
    rpiomx_graph_shutdown(&ctx);


    // 3.2 tear down all the graph 
    // @TODO: smaller parts
    rpiomx_graph_teardown(&ctx);

    
    // 3.3 remove IL components from openmax  
    // Free the component handles
    if((r = OMX_FreeHandle(ctx.camera)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free camera component handle");
    }
    if((r = OMX_FreeHandle(ctx.encoder)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free encoder component handle");
    }
    if((r = OMX_FreeHandle(ctx.null_sink)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free null sink component handle");
    }
#ifndef ORIGINAL
    if((r = OMX_FreeHandle(ctx.video_splitter)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free video_splitter component handle");
    }
    if((r = OMX_FreeHandle(ctx.resize)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free resize component handle");
    }
    if((r = OMX_FreeHandle(ctx.write_media)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free write_media component handle");
    }
#endif

    // 3.4 release all resources from system
    fclose(ctx.fd_out);

    vcos_semaphore_delete(&ctx.handler_lock);
    if((r = OMX_Deinit()) != OMX_ErrorNone) {
        omx_die(r, "OMX de-initalization failed");
    }

    // Exit
    say("Exit!");

    return 0;
}
