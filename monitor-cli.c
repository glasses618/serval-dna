/* 
Copyright (C) 2012 Paul Gardner-Stephen 

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

#include "serval.h"

char cmd[1024];
int cmdLen=0;
int cmdOfs=0;
int dataBytesExpected=0;
unsigned char data[65536];
int dataBytes=0;

#define STATE_CMD 1
#define STATE_DATA 2
int state=STATE_CMD;

int fd;
int writeLine(char *msg)
{
  write(fd,msg,strlen(msg));
  return 0;
}

int processChar(int c);

int autoAnswerP=1;
int pipeAudio=1;
int syntheticAudio=0;
int showReceived=1;
int interactiveP=1;
int recordCodec=VOMP_CODEC_PCM;
int recordCodecBlockSamples=320;
int recordCodecTimespan=20;
int callSessionToken=0;

int app_monitor_cli(int argc, const char *const *argv, struct command_line_option *o)
{
  const char *sid=NULL;
  cli_arg(argc, argv, o, "sid", &sid, NULL, "");
  struct sockaddr_un addr;

  if ( (fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket");
    exit(-1);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  addr.sun_path[0]=0;
  snprintf(&addr.sun_path[1],100,"org.servalproject.servald.monitor.socket");
  int len = 1+strlen(&addr.sun_path[1]) + sizeof(addr.sun_family);
  char *p=(char *)&addr;
  printf("last char='%c' %02x\n",p[len-1],p[len-1]);

  if (connect(fd, (struct sockaddr*)&addr, len) == -1) {
    perror("connect");
    exit(-1);
  }

  if (pipeAudio) {
    detectAudioDevice();
    char *name=detectedAudioDeviceName;
    if (!name) {
      WHY("Could not detect any audio device. Will not pipe audio.");
      pipeAudio=0;
    }
  }

  struct pollfd fds[128];
  int fdcount=0;

  fds[fdcount].fd=fd;
  fds[fdcount].events=POLLIN;
  fdcount++;
  if (interactiveP) {
    fds[fdcount].fd=STDIN_FILENO;
    fds[fdcount].events=POLLIN;
    fdcount++;
  }  

  writeLine("monitor vomp\n");
  writeLine("monitor rhizome\n");

  if (sid!=NULL&&sid[0]) {
    char msg[1024];
    snprintf(msg,1024,"call %s 5551 5552\n",argv[1]);
    writeLine(msg);
  }

  char line[1024];
  /* Allow for up to one second of audio read from the microphone
     to be buffered. This is probably more than we will ever need.
     The primary purpose of the buffer is in fact to handle the fact
     that we are unlikely to ever read exaclty the number of samples
     we need, so we need to keep any left over ones from the previous
     read. */
  int audioRecordBufferBytes=0;
  unsigned char audioRecordBuffer[8000*2];

  int base_fd_count=fdcount;
  while(1) {
    fdcount=base_fd_count;
    int audio_fd = getAudioRecordFd();
    if (audio_fd>-1) {
      fds[fdcount].fd=STDIN_FILENO;
      fds[fdcount].events=POLLIN;
      fdcount++;
    }
    poll(fds,fdcount,1000);

    fcntl(fd,F_SETFL,
	  fcntl(fd, F_GETFL, NULL)|O_NONBLOCK);
    if (interactiveP) 
      fcntl(STDIN_FILENO,F_SETFL,
	    fcntl(STDIN_FILENO, F_GETFL, NULL)|O_NONBLOCK);
    if (audio_fd>-1) 
      fcntl(audio_fd,F_SETFL,
	    fcntl(audio_fd, F_GETFL, NULL)|O_NONBLOCK);
    
    int bytes;
    int i;
    line[0]=0;
    bytes=read(fd,line,1024);
    if (bytes>0)
      for(i=0;i<bytes;i++) processChar(line[i]);
    if (interactiveP) {
      bytes=read(STDIN_FILENO,line,1024);
      if (bytes>0) {
	line[bytes]=0;
	printf("< %s",line);
	write(fd,line,bytes);
      }
      if (audio_fd>-1) {
	/* attempt to read next audio buffer.
	   Some audio devices that we are using require an entire buffer
	   to be read at a time. */
	audioRecordBufferBytes
	  =getAudioBytes(audioRecordBuffer,audioRecordBufferBytes,sizeof(audioRecordBuffer));
	/* 8KHz 16 bit samples = 16000 bytes per second.
	   Thus one 1ms of audio = 16 bytes. */
	int audioRecordBufferOffset=0;
	while ((audioRecordBufferBytes-audioRecordBufferOffset)
	       >recordCodecTimespan*16) {
	  /* encode and deliver audio block to servald via monitor interface */
	  encodeAndDispatchRecordedAudio(fd,callSessionToken,recordCodec,
					 &audioRecordBuffer[audioRecordBufferOffset],
					 recordCodecTimespan*16);			      
	  /* skip over the samples we have already processed */
	  audioRecordBufferOffset+=recordCodecTimespan*16;
	}
	/* copy the remaining buffered bytes down and correct buffer length */
	if (audioRecordBufferOffset<0) audioRecordBufferOffset=0;
	if (audioRecordBufferOffset>audioRecordBufferBytes)
	  audioRecordBufferOffset=audioRecordBufferBytes;
	bcopy(&audioRecordBuffer[audioRecordBufferOffset],
	      &audioRecordBuffer[0],
	      audioRecordBufferBytes-audioRecordBufferOffset);
	audioRecordBufferBytes-=audioRecordBufferOffset;
      }
    }

  fcntl(fd,F_SETFL,
	fcntl(fd, F_GETFL, NULL)&~O_NONBLOCK);
  fcntl(STDIN_FILENO,F_SETFL,
	fcntl(STDIN_FILENO, F_GETFL, NULL)&~O_NONBLOCK);

  }
  
  return 0;
}

int counter=0;
int callState=0;
int processLine(char *cmd,unsigned char *data,int dataLen)
{
  int l_id,r_id,l_state,r_state,codec;
  long long start_time,end_time;
  if (showReceived) {
    printf("> %s\n",cmd);
    if (data) {
      int i,j;
      for(i=0;i<dataLen;i+=16) {
	printf("   %04x :",i);
	for(j=0;j<16;j++) 
	  if (i+j<dataLen) printf(" %02x",data[i+j]); else printf("   ");
	printf("  ");
	for(j=0;j<16;j++) 
	  if (i+j<dataLen) {
	    if (data[i+j]>=0x20&&data[i+j]<0x7e)
	      printf("%c",data[i+j]); else printf(".");
	  }
	printf("\n");
      }
    }
  }
  if (sscanf(cmd,"AUDIOPACKET:%x:%x:%d:%d:%d:%lld:%lld",
	     &l_id,&r_id,&l_state,&r_state,
	     &codec,&start_time,&end_time)==7)
    {
      if (pipeAudio) {
	playAudio(data,dataLen);
      }
    }
  if (sscanf(cmd,"CALLSTATUS:%x:%x:%d:%d",
	     &l_id,&r_id,&l_state,&r_state)==4)
    {
      if (l_state==4&&autoAnswerP) {
	// We are ringing, so pickup
	char msg[1024];
	sprintf(msg,"pickup %x\n",l_id);
	writeLine(msg);
      }
      if (l_state==5) { 
	startAudio();
	callSessionToken=l_id;
      } else {
	stopAudio();
	callSessionToken=0;
      }
      callState=l_state;
    }
  if (sscanf(cmd,"KEEPALIVE:%x",&l_id)==1) {
    if (callState==5&&syntheticAudio) {
	/* Send synthetic audio packet */
	char buffer[1024];
	sprintf(buffer,"*320:AUDIO:%x:8\n"
		"%08d pasdfghjklzxcvbnm123456"
		"qwertyuiopasdfghjklzxcvbnm123456"
		"qwertyuiopasdfghjklzxcvbnm123456"
		"qwertyuiopasdfghjklzxcvbnm123456"
		"qwertyuiopasdfghjklzxcvbnm123456"
		"qwertyuiopasdfghjklzxcvbnm123456"
		"qwertyuiopasdfghjklzxcvbnm123456"
		"qwertyuiopasdfghjklzxcvbnm123456"
		"qwertyuiopasdfghjklzxcvbnm123456"
		"qwertyuiopasdfghjklzxcvbnm123456",l_id,counter++);
	writeLine(buffer);
	printf("< *320:AUDIO:%x:8\\n<320 bytes>\n",l_id);
      }
  }
  cmd[0]=0;
  cmdLen=0;
  dataBytes=0;
  dataBytesExpected=0;
  state=STATE_CMD;
  return 0;
}
int processChar(int c)
{
  switch(state) {
  case STATE_CMD:
    if (c!='\n') {
      if (cmdLen<1000) {
	cmd[cmdLen++]=c;
      }
    } else {
      if (!cmdLen) return 0;
      cmd[cmdLen]=0;
      if (sscanf(cmd,"*%d:%n",&dataBytesExpected,&cmdOfs)==1) {
	if (dataBytesExpected<0) dataBytesExpected=0;
	if (dataBytesExpected>65535) dataBytesExpected=65535;
	state=STATE_DATA;
      } else {
	processLine(cmd,NULL,0);
	cmdLen=0;
      }
    }
    break;
  case STATE_DATA:
    if (dataBytes<dataBytesExpected)
      data[dataBytes++]=c;
    if (dataBytes>=dataBytesExpected) {
      processLine(&cmd[cmdOfs],data,dataBytes);
      cmdLen=0;
    }
  }      
  return 0;
}