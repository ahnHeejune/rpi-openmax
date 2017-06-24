CC = gcc
C++ = g++
# build setting

CFLAGS = -g -O2 -Wall -I.                # if compler error, check FFMPEG and SDL installation

LDFLAGS1 = -lavcodec -lavutil -lavformat  # if FFMPEG needed
LDFLAGS2 =-lSDL -lSDLmain     	 # if SDL needed

TARGET = ff264f2yuv 
TARGET += yuvviewer 

OBJS1 = ff264f2yuv.o ff264dec.o
OBJS2 = yuvviewer.o 

all: $(TARGET)

ff264f2yuv: $(OBJS1) 
	$(CC) $(LDFLAGS1) $(OBJS1) -o $@ 

yuvviewer: $(OBJS2) 
	$(CC) $(LDFLAGS2) $(OBJS2) -o $@ 

# rule for C files
%.o:%.c 
	$(CC) -c $(CFLAGS) $<  
# rule for C++ files
%.o:%.cpp 
	$(C++) -c $(CFLAGS) $<  

clean:
	rm -f $(OBJS) $(TARGET)

allclean:
	rm -f $(OBJS) $(TARGET) *.yuv

