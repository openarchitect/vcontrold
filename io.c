/*
 * io.c Kommunikation mit der Vito* Steuerung 
 *  */
/* $Id: io.c 34 2008-04-06 19:39:29Z marcust $ */
#include <stdlib.h>
#include <stdio.h> 
#include <errno.h>
#include <signal.h>  
#include <syslog.h> 
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/ioctl.h>

#include "io.h"
#include "socket.h"
#include "common.h"

#ifdef __CYGWIN__
/* NCC is not defined under cygwin */
#define NCC NCCS
#endif

static void sig_alrm(int);
static jmp_buf	env_alrm;

void closeDevice(int fd) {
	close(fd);
}

int openDevice(char *device) {
	/* wir unterscheiden hier TTY und Socket Verbindung */
	/* Socket: kein / am Anfang und ein : */
	int fd;
	char *dptr;

	if (device[0] != '/' && (dptr=strchr(device,':'))) {
		char host[MAXBUF];
		int port;
		port=atoi(dptr+1);
		/* dptr-device liefert die Laenge des Hostes */
		bzero(host,sizeof(host));
		strncpy(host,device,dptr-device);
		/* 3. Parameter ==1 --->noTCPDelay wird gesetzt */
		fd=openCliSocket(host,port,1);
	}	
	else 
		fd=opentty(device);
	if (fd <0 ) {
		/* hier kann noch Fehlerkram rein */
		return(-1);
	}
	return(fd);
}


int opentty(char *device) {
	int fd;

	logIT(LOG_DEBUG, "configure serial port %s",device);
	if ((fd=open(device,O_RDWR)) < 0) {
		logIT(LOG_ERR,"cannot open %s:%m",device);
		exit(1);
	}
	logIT(LOG_DEBUG, "serial port %s open",device);
	int s;
	struct termios oldsb, newsb;
	logIT(LOG_DEBUG, "Loading current configuration",device);
	s=tcgetattr(fd,&oldsb);

	if (s<0) {
		logIT(LOG_ERR,"error tcgetattr %s:%m",device);
		exit(1);
	}
	newsb=oldsb;

#ifdef NCC
	/* NCC is not always defined */
	for (s = 0; s < NCC; s++)
		newsb.c_cc[s] = 0;
#else
	bzero (&newsb, sizeof(newsb));
#endif
	newsb.c_iflag=IGNBRK | IGNPAR;
	newsb.c_oflag = 0;
	newsb.c_lflag = 0;   /* removed ISIG for susp=control-z problem; */
	newsb.c_cflag = (CLOCAL | B4800 | CS8 | CREAD| PARENB | CSTOPB);
	newsb.c_cc[VMIN]   = 1;
	newsb.c_cc[VTIME]  = 0;

	logIT(LOG_DEBUG, "Loading new configuration",device);
	tcsetattr(fd, TCSADRAIN, &newsb);

	/* DTR High fuer Spannungsversorgung */
	int modemctl = 0;
	ioctl(fd, TIOCMGET, &modemctl);
	modemctl |= TIOCM_DTR;
	s=ioctl(fd,TIOCMSET,&modemctl);
	if (s<0) {
		logIT(LOG_ERR,"error ioctl TIOCMSET %s:%m",device);
		exit(1);
	}

	logIT(LOG_DEBUG, "Serial port %s configured",device);
	return(fd);
}

int my_send(int fd,char *s_buf, int len) {
	int i;
	char string[256];

	/* Buffer leeren */
	/* da tcflush nicht richtig funktioniert, verwenden wir nonblocking read */
	fcntl(fd,F_SETFL,O_NONBLOCK);
	while(readn(fd,string,sizeof(string))>0);
	fcntl(fd,F_SETFL,!O_NONBLOCK);
	
	tcflush(fd,TCIFLUSH);
	
	/* wir benutzen die Socket feste Vairante aus socket.c */
	if(writen(fd,s_buf,len)!=len){
		logIT1(LOG_ERR,"io - my_send - writen fail");
		return (0);
	}
	for (i=0;i<len;i++) {
		
		unsigned char byte=s_buf[i] & 255;
		logIT(LOG_INFO,">SEND: %02X",(int)byte);
	}
	return(1);
}

int receive(int fd,char *r_buf,int r_len,unsigned long *etime) {
	int i;

	struct tms tms_t;
	clock_t start,end,mid,mid1;
	unsigned long clktck;
	clktck=sysconf(_SC_CLK_TCK);
	start=times(&tms_t);
	mid1=start;
	logIT1(LOG_DEBUG, "io - recieve - 1 - start reading");
	for(i=0;i<r_len;i++) {
		if (signal(SIGALRM, sig_alrm) == SIG_ERR){
			logIT1(LOG_ERR,"SIGALRM error");
		}
		logIT1(LOG_DEBUG, "io - recieve - 2 - setjmp");
		if(setjmp(env_alrm) !=0) {
			logIT1(LOG_ERR,"read timeout");
			return(-1);
		}
		alarm(TIMEOUT);
		/* wir benutzen die Socket feste Vairante aus socket.c */
		logIT1(LOG_DEBUG, "io - recieve - 3 - reading");
		if (readn(fd,&r_buf[i],1) <= 0) {
			logIT1(LOG_ERR,"error read tty");;
			alarm(0);
			return(-1);
		}
		alarm(0);
		unsigned char byte=r_buf[i] & 255;
		mid=times(&tms_t);
		logIT(LOG_INFO,"<RECV: %02X (%0.1f ms)", byte,((float)(mid-mid1)/clktck)*1000);
		mid1=mid;
	}
	end=times(&tms_t);
	*etime=((float)(end-start)/clktck)*1000;
	return i;
}

static int setnonblock(int fd) {
	int flags;
	/* If they have O_NONBLOCK, use the Posix way to do it */
#if defined(O_NONBLOCK)
	if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
		flags = 0;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
	/* Otherwise, use the old way of doing it */
	flags = 1;
	return ioctl(fd, FIOBIO, &flags);
#endif
}

static int setblock(int fd) {
	int flags;
	/* If they have O_BLOCK, use the Posix way to do it */
#if defined(O_NONBLOCK)
	if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
		flags = 0;
	return fcntl(fd, F_SETFL, flags & (~O_NONBLOCK));
#else
	/* Otherwise, use the old way of doing it */
	flags = 0;
	return ioctl(fd, FIOBIO, &flags);
#endif
}

static char * dump( char *dest, char *title, char *buf, int len) {
	int pos = 0;
	int i;

	pos = sprintf(dest, "%s", title);
	for ( i=0; i<len; i++)
	{
		pos+=sprintf(dest+pos, " %02X",  buf[i] & 0xff);
	}
	return dest;
}

int receive_nb(int fd,char *r_buf,int r_len, unsigned long *etime) {
	int i;
	ssize_t len;
	char string[100];

    fd_set rfds;
    struct timeval tv;
    int retval;

	struct tms tms_t;
	clock_t start,end,mid,mid1;
	unsigned long clktck;
	clktck=sysconf(_SC_CLK_TCK);
	start=times(&tms_t);
	mid1=start;

    i = 0;
    while ( i < r_len ) {
    	setnonblock(fd);
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = TIMEOUT;
        tv.tv_usec = 0;
    	retval = select(fd+1, &rfds, NULL, NULL, &tv);
    	if (retval == 0 ) {
			logIT(LOG_ERR,"<RECV: read timeout");
			setblock(fd);
			logIT(LOG_INFO,dump(string, "<RECV: received", r_buf, i));
			return(-1);
    	} else if (retval < 0) {
			if (errno == EINTR) {
				logIT(LOG_INFO, "<RECV: select interrupted - redo");
				continue;
			} else {
				logIT(LOG_ERR, "<RECV: select error %d", retval);
				setblock(fd);
				logIT(LOG_INFO,dump(string, "<RECV: received", r_buf, i));
				return (-1);
			}
		} else if ( FD_ISSET(fd, &rfds)){
			len = read(fd, &r_buf[i], r_len - i);
			if (len == 0) {
				logIT(LOG_ERR, "<RECV: read eof");
				setblock(fd);
				logIT(LOG_INFO,dump(string, "<RECV: received", r_buf, i));
				return (-1);
			} else if (len < 0) {
				if (errno == EINTR) {
					logIT(LOG_INFO, "<RECV: read interrupted - redo");
					continue;
				} else {
					logIT(LOG_ERR, "<RECV: read error %d", errno);
					setblock(fd);
					logIT(LOG_INFO,dump(string, "<RECV: received", r_buf, i));
					return (-1);
				}
			} else {
				unsigned char byte=r_buf[i] & 255;
				mid=times(&tms_t);
				logIT(LOG_INFO, "<RECV: len=%zd %02X (%0.1f ms)", len, byte, ((float)(mid-mid1)/clktck)*1000);
				mid1=mid;
				i += len;
			}
		} else {
			continue;
		}
	}
	end=times(&tms_t);
	*etime=((float)(end-start)/clktck)*1000;
	setblock(fd);
	logIT(LOG_INFO,dump(string, "<RECV: received", r_buf, i));
	return i;
}

static void sig_alrm(int signo) {
	longjmp(env_alrm,1);
}

int waitfor(int fd, char *w_buf,int w_len) {
	int i;
	time_t start;
	char r_buf[MAXBUF];
	char hexString[128]="\0";
	char dummy[3];
	unsigned long etime;
	for(i=0;i<w_len;i++) {
		sprintf(dummy,"%02X",w_buf[i]);	
		strncat(hexString,dummy,999);
	}
	
	logIT(LOG_INFO,"Waiting for %s",hexString);
	start=time(NULL);
	
	/* wir warten auf das erste Zeichen, danach muss es passen */
	do {
		etime=0;
		if (receive(fd,r_buf,1,&etime)<0) 
			return(0);
		if (time(NULL)-start > TIMEOUT) {
			logIT1(LOG_WARNING,"Timeout wait");
			return(0);
		}
	} while (r_buf[0] != w_buf[0]);
	for(i=1;i<w_len;i++) {
		etime=0;
		if (receive(fd,r_buf,1,&etime)<0) 
			return(0);
		if (time(NULL)-start > TIMEOUT) {
			logIT1(LOG_WARNING,"Timeout wait");
			return(0);
		}
		if( r_buf[0] != w_buf[i]) {
			logIT1(LOG_ERR,"Lost synchronization");
			exit(1);
		}
        }		
/*	logIT1(LOG_INFO,"Zeichenkette erkannt"); */
	return(1);
}
