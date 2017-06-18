#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <unistd.h> /* close() */
#include <string.h> /* memset() */
#include <pthread.h>

/*--------------------------------------------------------------------------
   DESC


   TCP cli for controling (video) streaming start and end 
   UDP cli for recieving  (video) data 

   main                 : listen socket 
     |               
   stream_control  	: tcp control socket 
     | 
   stream_loop          : udp stream (separate thread) 

---------------------------------------------------------------------------*/ 
/* GLOBAL -----------------------------------------------------------------*/


/* STATIC -----------------------------------------------------------------*/
static volatile int gStreamStopReq = 0;  // multitheaded  

#define LOCAL_SERVER_PORT  1500
#define STREAM_CLIENT_PORT 1501   
#define MAX_MSG 100
 
/* LOCAL ------------------------------------------------------------------*/



/* UDP streamer ----------------------------------------------------------

   send UDP packets (now dummy data, later video)
   IN:  arg (dest udp socket address)  
   GLOBAL: use gStreamStopReq 
   OUT: success or not  

*/
   
static void *stream_loop(void *arg) {
  

}

/* stream control module ----------------------------------------------- 
   control via TCP connection (to reliable communicaiton and detect connection loss)
   command : start streaming and stop
   response: ack and nack 
*/
static int stream_control(int sock) //, struct sockaddr_in *pCliAddr)
{

    return 0;
}



/*
 * open_clientfd - open connection to server at <hostname, port>
 *   and return a socket descriptor ready for reading and writing.
 *   Returns -1 and sets errno on Unix error.
 *   Returns -2 and sets h_errno on DNS (gethostbyname) error.
 */
int open_clientfd(char *hostname, int port)
{
    return clientfd;
}

/* $end open_clientfd */


/* main 

   main thread for control (tcp) server 
   command from client: 0x1 for start 0x0 for stop
   respnse to  client:  0x1 for ack   0x0 for nack (not used) 
*/
int main(int argc, char **argv)
{
    int clientfd, port;
    char *host;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        exit(0);
    }
    host = argv[1];
    port = atoi(argv[2]);

    clientfd = open_clientfd(host, port);

    fprintf(stdout, "command: 's' for start streaming, 'c' close streaming\n"); 

    stream_control(clientfd);

    close(clientfd);

    fprintf(stdout, "Bye!\n");

    return 0;
}


