/*
Copyright (C) 2010-2012 Paul Gardner-Stephen, Serval Project.
 
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

/*
  Android does unix domain sockets, but only stream sockets, not datagram sockets.
  So we need a separate monitor interface for Android. A bit of a pain, but in
  fact it lets us make a very Android/Java-friendly interface, without any binary
  data structures (except for a binary extent for an audio sample block).
*/

#include "serval.h"
#include "rhizome.h"
#include <sys/stat.h>

#if defined(LOCAL_PEERCRED) && !defined(SO_PEERCRED)
#define SO_PEERCRED LOCAL_PEERCRED
#endif

#define MONITOR_LINE_LENGTH 160
#define MONITOR_DATA_SIZE MAX_AUDIO_BYTES
struct monitor_context {
  struct sched_ent alarm;
  int flags;
  char line[MONITOR_LINE_LENGTH];
  int line_length;
#define MONITOR_STATE_COMMAND 1
#define MONITOR_STATE_DATA 2
  int state;
  unsigned char buffer[MONITOR_DATA_SIZE];
  int data_expected;
  int data_offset;
  int sample_codec;
  int sample_call_session_token;
};

#define MAX_MONITOR_SOCKETS 8
int monitor_socket_count=0;
struct monitor_context monitor_sockets[MAX_MONITOR_SOCKETS];

int monitor_process_command(struct monitor_context *c);
int monitor_process_data(struct monitor_context *c);
static void monitor_new_client(int s);

struct sched_ent named_socket;
struct profile_total named_stats;
struct profile_total client_stats;

int monitor_setup_sockets()
{
  struct sockaddr_un name;
  int len;
  int sock;
  
  bzero(&name, sizeof(name));
  name.sun_family = AF_UNIX;
  
  if ((sock = socket(AF_UNIX, SOCK_STREAM, 0))==-1) {
    WHY_perror("socket");
    goto error;
  }

#ifdef linux
  /* Use abstract namespace as Android has no writable FS which supports sockets.
     Abstract namespace is just plain better, anyway, as no dead files end up
     hanging around. */
  name.sun_path[0]=0;
  /* XXX: 104 comes from OSX sys/un.h - no #define (note Linux has UNIX_PATH_MAX and it's 108(!)) */
  snprintf(&name.sun_path[1],104-2,
	   confValueGet("monitor.socket",DEFAULT_MONITOR_SOCKET_NAME));
  /* Doesn't include trailing nul */
  len = 1+strlen(&name.sun_path[1]) + sizeof(name.sun_family);
#else
  snprintf(name.sun_path,104-1,"%s/%s",
	   serval_instancepath(),
	   confValueGet("monitor.socket",DEFAULT_MONITOR_SOCKET_NAME));
  unlink(name.sun_path);
  /* Includes trailing nul */
  len = 1+strlen(name.sun_path) + sizeof(name.sun_family);
#endif

  if(bind(sock, (struct sockaddr *)&name, len)==-1) {
    WHY_perror("bind");
    goto error;
  }
  if(listen(sock,MAX_MONITOR_SOCKETS)==-1) {
    WHY_perror("listen");
    goto error;
  }

  int reuseP=1;
  if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, 
		&reuseP, sizeof(reuseP)) < 0) {
    WHY_perror("setsockopt");
    WHY("Could not indicate reuse addresses. Not necessarily a problem (yet)");
  }
  
  int send_buffer_size=64*1024;    
  if(setsockopt(sock, SOL_SOCKET, SO_RCVBUF, 
		&send_buffer_size, sizeof(send_buffer_size))==-1)
    WHY_perror("setsockopt");
  if (debug&(DEBUG_IO|DEBUG_VERBOSE_IO)) DEBUG("Monitor server socket setup");

  named_socket.function=monitor_poll;
  named_stats.name="monitor_poll";
  named_socket.stats=&named_stats;
  named_socket.poll.fd=sock;
  named_socket.poll.events=POLLIN;
  watch(&named_socket);
  return 0;
  
  error:
  close(sock);
  return -1;
}

void monitor_poll(struct sched_ent *alarm)
{
  int s;
  unsigned char buffer[1024];
  struct sockaddr *ignored_address=(struct sockaddr *)&buffer[0];
  socklen_t ignored_length=sizeof(ignored_address);

  /* Check for new connections */
  /* We don't care about the peer's address */
  ignored_length = 0;
  while (
#ifdef HAVE_LINUX_IF_H
	 (s = accept4(alarm->poll.fd, NULL, &ignored_length,O_NONBLOCK))
#else
	 (s = accept(alarm->poll.fd,NULL, &ignored_length))
#endif
      != -1
  ) {
    monitor_new_client(s);
  }
  if (errno != EAGAIN) {
#ifdef HAVE_LINUX_IF_H
    WHY_perror("accept4(O_NONBLOCK)");
#else
    WHY_perror("accept");
#endif
  }
}

void monitor_client_close(struct monitor_context *c){
  struct monitor_context *last;
  
  unwatch(&c->alarm);
  close(c->alarm.poll.fd);
  c->alarm.poll.fd=-1;
  
  monitor_socket_count--;
  last = &monitor_sockets[monitor_socket_count];
  if (last != c){
    unwatch(&last->alarm);
    bcopy(last, c,
	  sizeof(struct monitor_context));
    watch(&c->alarm);
  }
}

void monitor_client_poll(struct sched_ent *alarm)
{
  /* Read from any open connections */
  struct monitor_context *c=(struct monitor_context *)alarm;
  errno=0;
  int bytes;
  
  switch(c->state) {
  case MONITOR_STATE_COMMAND:
    bytes = 1;
    while(bytes == 1) {
      if (c->line_length >= MONITOR_LINE_LENGTH) {
	/* line too long */
	c->line[MONITOR_LINE_LENGTH-1] = 0;
	monitor_process_command(c);
	bytes = -1;
	break;
      }
      bytes = read(c->alarm.poll.fd, &c->line[c->line_length], 1);
      if (bytes < 1) {
	switch(errno) {
	case EINTR:
	case ENOTRECOVERABLE:
	  /* transient errors */
	  WHY_perror("read");
	  break;
	case EAGAIN:
	  break;
	default:
	  WHY_perror("read");
	  /* all other errors; close socket */
	  INFO("Tearing down monitor client");
	  monitor_client_close(c);
	  return;
	}
      }
      if (bytes > 0 && (c->line[c->line_length] != '\r')) {
	  c->line_length += bytes;
	  if (c->line[c->line_length-1] == '\n') {
	    /* got command */
	    c->line[c->line_length-1] = 0; /* trim new line for easier parsing */
	    monitor_process_command(c);
	    break;
	  }
	}
    }
    break;
  case MONITOR_STATE_DATA:
    bytes = read(c->alarm.poll.fd,
		 &c->buffer[c->data_offset],
		 c->data_expected-c->data_offset);
    if (bytes < 1) {
      switch(errno) {
      case EAGAIN: case EINTR: 
	/* transient errors */
	break;
      default:
	/* all other errors; close socket */
	  WHYF("Tearing down monitor client due to errno=%d",
	       errno);
	  monitor_client_close(c);
	  return;
      }
    } else {
      c->data_offset += bytes;
      if (c->data_offset >= c->data_expected)
	{
	  /* we have the binary data we were expecting. */
	  monitor_process_data(c);
	  c->state = MONITOR_STATE_COMMAND;
	}
    }
    break;
  default:
    c->state = MONITOR_STATE_COMMAND;
    WHY("fixed monitor connection state");
  }
  return;
}
 
static void monitor_new_client(int s) {
#ifdef linux
  struct ucred			ucred;
  socklen_t			len;
  int				res;
#else
  gid_t				othergid;
#endif
  uid_t				otheruid;
  struct monitor_context	*c;

  SET_NONBLOCKING(s);

#ifdef linux
  len = sizeof(ucred);
  res = getsockopt(s, SOL_SOCKET, SO_PEERCRED, &ucred, &len);
  if (res) { 
    WHY_perror("getsockopt(SO_PEERCRED)");
    goto error;
  }
  if (len < sizeof(ucred)) {
    WHYF("getsockopt(SO_PEERCRED) returned the wrong size (Got %d expected %d)", len, sizeof(ucred));
    goto error;
  }
  otheruid = ucred.uid;
#else
  if (getpeereid(s, &otheruid, &othergid) != 0) {
    WHY_perror("getpeereid()");
    goto error;
  }
#endif

  if (otheruid != getuid()) {
    WHYF("monitor.socket client has wrong uid (%d versus %d)", otheruid,getuid());
    WRITE_STR(s, "\nCLOSE:Incorrect UID\n");
    goto error;
  }
  if (monitor_socket_count >= MAX_MONITOR_SOCKETS
	     ||monitor_socket_count < 0) {
    WRITE_STR(s, "\nCLOSE:All sockets busy\n");
    goto error;
  }
  
  c = &monitor_sockets[monitor_socket_count++];
  c->alarm.function = monitor_client_poll;
  client_stats.name = "monitor_client_poll";
  c->alarm.stats=&client_stats;
  c->alarm.poll.fd = s;
  c->alarm.poll.events=POLLIN;
  c->line_length = 0;
  c->state = MONITOR_STATE_COMMAND;
  WRITE_STR(s,"\nMONITOR:You are talking to servald\n");
  INFOF("Got %d clients", monitor_socket_count);
  watch(&c->alarm);  
  
  return;
  
  error:
    close(s);
    return;
}

int monitor_process_command(struct monitor_context *c) 
{
  int callSessionToken,sampleType,bytes;
  char sid[MONITOR_LINE_LENGTH],localDid[MONITOR_LINE_LENGTH];
  char remoteDid[MONITOR_LINE_LENGTH],digits[MONITOR_LINE_LENGTH];
  overlay_mdp_frame mdp;
  char *cmd = c->line;
  IN();
  
  mdp.packetTypeAndFlags=MDP_VOMPEVENT;  

  c->line_length=0;

  if (strlen(cmd)>MONITOR_LINE_LENGTH) {
    WRITE_STR(c->alarm.poll.fd,"\nERROR:Command too long\n");
    RETURN(-1);
  }

  char msg[1024];
  int flag;

  if (cmd[0]=='*') {
    /* command with content */
    int ofs=0;
    if (sscanf(cmd,"*%d:%n",&bytes,&ofs)==1) {
      /* work out rest of command */
      cmd=&cmd[ofs];
      c->state=MONITOR_STATE_DATA;
      c->data_expected=bytes;
      c->data_offset=0;
      c->sample_codec=-1;

      if (sscanf(cmd,"AUDIO:%x:%d",
		 &callSessionToken,&sampleType)==2)
	{
	  /* Start getting sample */
	  c->sample_call_session_token=callSessionToken;
	  c->sample_codec=sampleType;
	  RETURN(0);
	}
    }
  }
  else if (!strcasecmp(cmd,"monitor vomp"))
    c->flags|=MONITOR_VOMP;
  else if (!strcasecmp(cmd,"ignore vomp"))
    c->flags&=~MONITOR_VOMP;
  else if (!strcasecmp(cmd,"monitor rhizome"))
    c->flags|=MONITOR_RHIZOME;
  else if (!strcasecmp(cmd,"ignore rhizome"))
    c->flags&=~MONITOR_RHIZOME;
  else if (!strcasecmp(cmd,"monitor peers"))
    c->flags|=MONITOR_PEERS;
  else if (!strcasecmp(cmd,"ignore peers"))
    c->flags&=~MONITOR_PEERS;
  else if (sscanf(cmd,"FASTAUDIO:%x:%d",&callSessionToken,&flag)==2)
    {
      int i;
      for(i=0;i<vomp_call_count;i++)
	if (vomp_call_states[i].local.session==callSessionToken
	    ||callSessionToken==0) {
	  vomp_call_states[i].fast_audio=flag;
	  vomp_call_states[i].local.last_state=-1;
	  monitor_call_status(&vomp_call_states[i]);	  
	}
    }
  else if (sscanf(cmd,"call %s %s %s",sid,localDid,remoteDid)==3) {
    DEBUG("here");
    if (sid[0]=='*') {
      /* For testing, pick a peer and call them */
      int bin,slot;
      for(bin=0;bin<overlay_bin_count;bin++)
	for(slot=0;slot<overlay_bin_size;slot++)
	  {
	    if (!overlay_nodes[bin][slot].sid[0])
	      continue;
	    tohex(sid, overlay_nodes[bin][slot].sid, SID_SIZE);
	    break;
	  }
    }
    mdp.vompevent.flags=VOMPEVENT_DIAL;
    int cn=0,in=0,kp=0;
    if(!keyring_next_identity(keyring,&cn,&in,&kp))
      {
	WRITE_STR(c->alarm.poll.fd,"\nERROR:no local identity, so cannot place call\n");
      }
    else {
      bcopy(keyring->contexts[cn]->identities[in]->keypairs[kp]->public_key,
	    &mdp.vompevent.local_sid[0], SID_SIZE);
      stowSid(&mdp.vompevent.remote_sid[0],0,sid);
      vomp_mdp_event(&mdp,NULL,0);
    }
    DEBUG("here");
  } 
  else if (sscanf(cmd,"status %x",&callSessionToken)==1) {
    int i;
    for(i=0;i<vomp_call_count;i++)
      if (vomp_call_states[i].local.session==callSessionToken
	  ||callSessionToken==0) {
	vomp_call_states[i].local.last_state=0;
	monitor_call_status(&vomp_call_states[i]);
      }
  } else if (sscanf(cmd,"pickup %x",&callSessionToken)==1) {
     mdp.vompevent.flags=VOMPEVENT_PICKUP;
     mdp.vompevent.call_session_token=callSessionToken;
     vomp_mdp_event(&mdp,NULL,0);
  }
  else if (sscanf(cmd,"hangup %x",&callSessionToken)==1) {
     mdp.vompevent.flags=VOMPEVENT_HANGUP;
     mdp.vompevent.call_session_token=callSessionToken;
     vomp_mdp_event(&mdp,NULL,0);
  } else if (sscanf(cmd,"dtmf %x %s",&callSessionToken,digits)==2) {
    mdp.vompevent.flags=VOMPEVENT_AUDIOPACKET;
    mdp.vompevent.call_session_token=callSessionToken;

    /* One digit per sample block. */
    mdp.vompevent.audio_sample_codec=VOMP_CODEC_DTMF;
    mdp.vompevent.audio_sample_bytes=1;
    
    int i;
    for(i=0;i<strlen(digits);i++) {
      int digit=vomp_parse_dtmf_digit(digits[i]);
      if (digit<0) {
	snprintf(msg,1024,"\nERROR: invalid DTMF digit 0x%02x\n",digit);
	WRITE_STR(c->alarm.poll.fd,msg);
      }
      mdp.vompevent.audio_bytes[mdp.vompevent.audio_sample_bytes]
	=(digit<<4); /* 80ms standard tone duration, so that it is a multiple
			of the majority of codec time units (70ms is the nominal
			DTMF tone length for most systems). */
      if (overlay_mdp_send(&mdp,0,0)) WHY("Send DTMF failed.");
    }
    
  }

  snprintf(msg,1024,"\nMONITORSTATUS:%d\n",c->flags);
  WRITE_STR(c->alarm.poll.fd,msg);

  RETURN(0);
}

int monitor_process_data(struct monitor_context *c) 
{
  /* Called when we have received an entire data sample */
  c->state=MONITOR_STATE_COMMAND;

  if (vomp_sample_size(c->sample_codec)!=c->data_offset) {
      WARNF("Ignoring sample block of incorrect size (expected %d, got %d bytes for codec %d)",
	   vomp_sample_size(c->sample_codec), c->data_offset, c->sample_codec);
    return -1;
  }

  vomp_call_state *call=vomp_find_call_by_session(c->sample_call_session_token);
  if (!call) {
    WRITE_STR(c->alarm.poll.fd,"\nERROR:No such call\n");
    return -1;
  }

  overlay_mdp_frame mdp;
  mdp.packetTypeAndFlags=MDP_VOMPEVENT;
  mdp.vompevent.flags=VOMPEVENT_AUDIOPACKET;
  mdp.vompevent.call_session_token=c->sample_call_session_token;
  mdp.vompevent.audio_sample_codec=c->sample_codec;
  bcopy(&c->buffer[0],&mdp.vompevent.audio_bytes[0],
	vomp_sample_size(c->sample_codec));
  mdp.vompevent.audio_sample_bytes=vomp_sample_size(c->sample_codec);

  if (overlay_mdp_send(&mdp,0,0)) WARN("Send audio failed.");

  return 0;
}

int monitor_announce_bundle(rhizome_manifest *m)
{
  int i;
  char msg[1024];
  const char *service = rhizome_manifest_get(m, "service", NULL, 0);
  const char *sender = rhizome_manifest_get(m, "sender", NULL, 0);
  const char *recipient = rhizome_manifest_get(m, "recipient", NULL, 0);
  snprintf(msg,1024,"\nBUNDLE:%s:%s:%lld:%lld:%s:%s:%s\n",
	   /* XXX bit of a hack here, since SIDs and cryptosign public keys have the same length */
	   alloca_tohex_sid(m->cryptoSignPublic),
	   service ? service : "",
	   m->version,
	   m->fileLength,
	   sender,
	   recipient,
	   m->dataFileName?m->dataFileName:"");
  for(i=monitor_socket_count -1;i>=0;i--)
    {
      if (!(monitor_sockets[i].flags&MONITOR_RHIZOME))
	continue;
      errno=0;
      
      SET_NONBLOCKING(monitor_sockets[i].alarm.poll.fd);
      WRITE_STR(monitor_sockets[i].alarm.poll.fd,msg);
      
      SET_BLOCKING(monitor_sockets[i].alarm.poll.fd);
      
      if (errno&&(errno!=EINTR)&&(errno!=EAGAIN)) {
	/* error sending update, so kill monitor socket */
	WHY_perror("write");
	INFO("Tearing down monitor client");
	monitor_client_close(&monitor_sockets[i]);
      }
    }
  return 0;
}

int monitor_call_status(vomp_call_state *call)
{
  int i;
  char msg[1024];
  int show=0;
  IN();
  if (call->local.state>call->local.last_state) show=1;
  if (call->remote.state>call->remote.last_state) show=1;
  call->local.last_state=call->local.state;
  call->remote.last_state=call->remote.state;
  if (show) {
    if (0) DEBUG("sending call status to monitor");
    snprintf(msg,1024,"\nCALLSTATUS:%06x:%06x:%d:%d:%d:%s:%s:%s:%s\n",
	     call->local.session,call->remote.session,
	     call->local.state,call->remote.state,
	     call->fast_audio,
	     alloca_tohex_sid(call->local.sid),
	     alloca_tohex_sid(call->remote.sid),
	     call->local.did,call->remote.did);
    msg[1023]=0;
    for(i=monitor_socket_count -1;i>=0;i--)
      {
	if (!(monitor_sockets[i].flags&MONITOR_VOMP))
	  continue;
	errno=0;
	SET_NONBLOCKING(monitor_sockets[i].alarm.poll.fd);
	WRITE_STR(monitor_sockets[i].alarm.poll.fd,msg);
	SET_BLOCKING(monitor_sockets[i].alarm.poll.fd);
	if (errno&&(errno!=EINTR)&&(errno!=EAGAIN)) {
	  /* error sending update, so kill monitor socket */
	  WHY_perror("write");
	  INFOF("Tearing down monitor client #%d", i);
	  monitor_client_close(&monitor_sockets[i]);
	}
      }
  }
  RETURN(0);
}

int monitor_announce_peer(const unsigned char *sid)
{
  char msg[1024];
  int n = snprintf(msg, sizeof msg, "\nNEWPEER:%s\n", alloca_tohex_sid(sid));
  monitor_tell_clients(msg, n, MONITOR_PEERS);
  return 0;
}

int monitor_send_audio(vomp_call_state *call,overlay_mdp_frame *audio)
{
  if (0) DEBUGF("Tell call monitor about audio for call %06x:%06x",
	      call->local.session,call->remote.session);
  int sample_bytes=vomp_sample_size(audio->vompevent.audio_sample_codec);
  char msg[1024 + MAX_AUDIO_BYTES];
  /* All commands followed by binary data start with *len:, so that 
     they can be easily parsed at the far end, even if not supported.
     Put newline at start of these so that receiving data in command
     mode doesn't confuse the parser.  */
  int msglen = snprintf(msg, 1024,
	   "\n*%d:AUDIOPACKET:%06x:%06x:%d:%d:%d:%lld:%lld\n",
	   sample_bytes,
	   call->local.session,call->remote.session,
	   call->local.state,call->remote.state,
	   audio->vompevent.audio_sample_codec,
	   audio->vompevent.audio_sample_starttime,
	   audio->vompevent.audio_sample_endtime);
  bcopy(&audio->vompevent.audio_bytes[0], &msg[msglen], sample_bytes);
  msglen+=sample_bytes;
  msg[msglen++]='\n';
  monitor_tell_clients(msg, msglen, MONITOR_VOMP);
  return 0;
}

int monitor_tell_clients(char *msg, int msglen, int mask)
{
  int i;
  IN();
  for(i=monitor_socket_count -1;i>=0;i--)
    {
      if (!(monitor_sockets[i].flags&mask))
	continue;
      errno=0;
      SET_NONBLOCKING(monitor_sockets[i].alarm.poll.fd);
      write(monitor_sockets[i].alarm.poll.fd, msg, msglen);
      SET_BLOCKING(monitor_sockets[i].alarm.poll.fd);
      // WHYF("Writing AUDIOPACKET to client");
      if (errno&&(errno!=EINTR)&&(errno!=EAGAIN)) {
	/* error sending update, so kill monitor socket */
	WHY_perror("write");
	INFOF("Tearing down monitor client #%d", i);
	monitor_client_close(&monitor_sockets[i]);
      }
    }
  RETURN(0);
}
