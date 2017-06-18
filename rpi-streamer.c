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


   TCP server for control video streaming start and end 
   UDP server for sendind Video data 

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
  
  int sock, rc, n, cliLen, flags;
  struct sockaddr_in servAddr;
  char msg[MAX_MSG];
  short localport = LOCAL_SERVER_PORT;
  struct sockaddr_in cliAddr = *(struct sockaddr_in *)arg; // make a copy for modified

  /* 1. socket creation */
  sock=socket(AF_INET, SOCK_DGRAM, 0);
  if(sock<0) {
    fprintf(stderr, "Error:cannot open udp socket (%d)\n",localport);
    pthread_exit((void *)-1);
  }

  /* 2. bind local server port */
  servAddr.sin_family = AF_INET;
  servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servAddr.sin_port = htons(STREAM_CLIENT_PORT-1); // can use any not conflicting  
  rc = bind (sock, (struct sockaddr *) &servAddr,sizeof(servAddr));
  if(rc<0) {
    fprintf(stderr, "Error: cannot bind port number %d\n", localport);
    pthread_exit((void *)-1);
  }

  /* 3. prepare destination address */
  //cliAddr.sin_family = AF_INET;
  //cliAddr.sin_addr.s_addr = htonl(); // same destination as contoller 
  cliAddr.sin_port = htons(1501);      // different port 
  cliLen = sizeof(struct sockaddr_in);

  flags = 0;

  /* 4. infinite loop */
  while(gStreamStopReq != 1) {

    fprintf(stdout, "STREAM> Alive \n");
    
    /* init buffer with dummy data (0xF0) */
    memset(msg,0xF0,MAX_MSG);

    n = MAX_MSG; // temporary
    n = sendto(sock,msg,n,flags,(struct sockaddr *)&cliAddr,cliLen);
    if(n < MAX_MSG)
   	fprintf(stderr,"cannot send all data (%d) to client\n", n);

    usleep(1000000L); // slow down for debugging
    
  }/* end of server infinite loop */


  /* 5. finish */
  close(sock);
  pthread_exit((void *)0); // user-requested-stop

}

/* open a tcp server socket --------------------------------------------- 
 a wrapper function to hide dirty details 

*/ 

static int open_listenfd(short portNum)
{
  int sock, rc;
  struct sockaddr_in servAddr;

  sock=socket(AF_INET, SOCK_STREAM, 0);
  if(sock<0) {
    fprintf(stderr,"Error: cannot open socket (%d) \n",portNum);
    return -1; 
  }

  /* bind local server port */
  servAddr.sin_family = AF_INET;
  servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servAddr.sin_port = htons(portNum);
  rc = bind (sock, (struct sockaddr *) &servAddr, sizeof(servAddr));
  if(rc<0) {
    fprintf(stderr,"Error: cannot bind port number %d \n", portNum);
    return -1;
  }

  return sock; 
}

/* stream control module ----------------------------------------------- 
   control via TCP connection (to reliable communicaiton and detect connection loss)
   command : start streaming and stop
   response: ack and nack 
*/

static int stream_control(int sock, struct sockaddr_in *pCliAddr)
{
   size_t n;
   int r;
   pthread_t tid;
   int  retval;
   char rxbuf[128]; /* one byte only used */
   char txbuf[128]; /* one byte only used */
   int flags = 0;

   while(1){

	// 1. get commands from client
   	n = recv(sock, rxbuf, 128, flags);

	// 2. prorocol error check 
   	if (n<=0){
		fprintf(stderr, "read error: connection closed\n");
                gStreamStopReq = 1;
	  	r = pthread_join(tid, (void **)&retval);  
		gStreamStopReq = 0;
		return -1;  // abnormal finish 
   	}
   	else if(n > 1){ 
		fprintf(stderr, "protocol error: too big msg\n");
		txbuf[0]  = 'n'; // nack
		write(sock, txbuf, 1);
		continue;
   	}
	// 3. protocol handle
	else{

		switch(rxbuf[0]){

		case 's':
			r = pthread_create(&tid, NULL, stream_loop, (void *)pCliAddr);
                   	if(r!=0){
				fprintf(stderr, "ERROR:pthread_create\n");
				txbuf[0]  = 'n'; // ack
				write(sock, txbuf, 1);
			}else{
				txbuf[0]  = 'a'; // ack
				write(sock, txbuf, 1);
			}

			break;
		case 'c': // finish streaming
                   	gStreamStopReq = 1;
		   	r = pthread_join(tid, (void **)&retval); // TODO: check it run successfully 
			gStreamStopReq = 0;
		 	if(r!=0){
				fprintf(stderr, "ERROR:pthread_join\n");
		 		gStreamStopReq = 0;
				txbuf[0]  = 'n'; // ack
				write(sock, txbuf, 1);

			}else{
				txbuf[0]  = 'a'; // ack
				write(sock, txbuf, 1);
				return 0; // normal  finish 
			}

		default:
			txbuf[0]  = 'n'; // nack
			write(sock, txbuf, 1);
			break;
		}
	}
	
   }
}


/* main 

   main thread for control (tcp) server 
   command from client: 0x1 for start 0x0 for stop
   respnse to  client:  0x1 for ack   0x0 for nack (not used) 
*/
int main(int argc, char **argv)
{
    int listenfd, connfd, port;
    socklen_t  clientlen;
    struct sockaddr_in clientaddr;
    struct hostent *hp;
    char *haddrp;
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }
    port = atoi(argv[1]);

    listenfd = open_listenfd(port);
    listen(listenfd, 1);

    clientlen = sizeof(clientaddr);

    while (1) // to stop CTRL-C or kill me 
    {
        connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen);

        /* determine the domain name and IP address of the client */
        hp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr,
                           sizeof(clientaddr.sin_addr.s_addr), AF_INET);
        haddrp = inet_ntoa(clientaddr.sin_addr);
        fprintf(stderr, "CNTL> new client %s (%s) connected\n", hp->h_name, haddrp);

        stream_control(connfd, &clientaddr);

        fprintf(stderr, "CTNL> connection %s (%s) lost\n", hp->h_name, haddrp);

        close(connfd);
    }
    return 0;
}


