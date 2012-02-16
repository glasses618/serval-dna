/* 
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2010 Paul Gardner-Stephen 

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

#include "mphlr.h"

typedef struct command_line_option {
  int (*function)(int argc,char **argv,struct command_line_option *o);
  char *words[32]; // 32 words should be plenty!
  unsigned long long flags;
#define CLIFLAG_NONOVERLAY (1<<0) /* Uses a legacy IPv4 DNA call instead of overlay mnetwork */
#define CLIFLAG_STANDALONE (1<<1) /* Cannot be issued to a running instance */
  char *description; // describe this invocation
} command_line_option;

extern command_line_option command_line_options[];

int cli_usage() {
  fprintf(stderr,"\nServal Mesh version <version>.\n");
  fprintf(stderr,"Usage:\n");
  int i,j;
  for(i=0;command_line_options[i].function;i++)
    {
      for(j=0;command_line_options[i].words[j];j++)
	fprintf(stderr," %s",command_line_options[i].words[j]);
      fprintf(stderr,"\n   %s\n",command_line_options[i].description);
    }
  return -1;
}

/* args[] excludes command name (unless hardlinks are used to use first words 
   of command sequences as alternate names of the command. */
int parseCommandLine(int argc, char **args)
{
  int i,j;
  int ambiguous=0;
  int cli_call=-1;
  for(i=0;command_line_options[i].function;i++)
    {
      for(j=0;(j<argc)&&command_line_options[i].words[j];j++)
	if ((command_line_options[i].words[j][0]!='<')&&
	    strcasecmp(command_line_options[i].words[j],args[j])) {
	  /* Words don't match, and word is not a place-holder for an argument,
	     so it isn't this command line call. */
	  break;
	}

      if ((j==argc)&&(!command_line_options[i].words[j])) {
	/* We used up all words in args and command line call sequence, so we have
	   a match. If we have multiple matches, then note that the call is 
	   ambiguous. */
	if (cli_call>=0) ambiguous++;
	if (ambiguous==1) {
	  fprintf(stderr,"Ambiguous command line call:\n   ");
	  for(j=0;j<argc;j++) fprintf(stderr," %s",args[j]);
	  fprintf(stderr,"\nMatches the following known command line calls:\n");
	}
	if (ambiguous) {
	  fprintf(stderr,"   ");
	  for(j=0;j<argc;j++) fprintf(stderr," %s",command_line_options[i].words[j]);
	  fprintf(stderr,"\n");
	}
	cli_call=i;
      }
    }
  
  /* Don't process ambiguous calls */
  if (ambiguous) return -1;
  /* Complain if we found no matching calls */
  if (cli_call<0) return cli_usage();

  /* Otherwise, make call */
  return command_line_options[i].function(argc,args,
					  &command_line_options[i]);
}

int app_dna_lookup(int argc,char **argv,struct command_line_option *o)
{
  return -1;
}

/* NULL marks ends of command structure.
   "<anystring>" marks an arg that can take any value.
   Only exactly matching prototypes will be used.
   Together with the description, this makes it easy for us to auto-generate the
   list of valid command line formats for display to the user if they try an
   invalid one.  It also means we can do away with getopt() etc.

   Keep this list alphabetically sorted for user convenience.
*/
command_line_option command_line_options[]={
  {app_dna_lookup,{"dna","lookup","<did>",NULL},CLIFLAG_NONOVERLAY,"Lookup the SIP/MDP address of the supplied telephone number (DID)."},
  {cli_usage,{"help",NULL},0,"Display command usage."},
  {NULL,{NULL}}
};
