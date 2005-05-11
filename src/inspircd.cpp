/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  Inspire is copyright (C) 2002-2004 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *           	  <Craig@chatspike.net>
 *     
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

/* Now with added unF! ;) */

using namespace std;

#include "inspircd.h"
#include "inspircd_io.h"
#include "inspircd_util.h"
#include "inspircd_config.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#ifdef USE_KQUEUE
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#endif
#include <cstdio>
#include <time.h>
#include <string>
#ifdef GCC3
#include <ext/hash_map>
#else
#include <hash_map>
#endif
#include <map>
#include <sstream>
#include <vector>
#include <errno.h>
#include <deque>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include "connection.h"
#include "users.h"
#include "servers.h"
#include "ctables.h"
#include "globals.h"
#include "modules.h"
#include "dynamic.h"
#include "wildcard.h"
#include "message.h"
#include "mode.h"
#include "commands.h"
#include "xline.h"
#include "inspstring.h"
#include "dnsqueue.h"

#ifdef GCC3
#define nspace __gnu_cxx
#else
#define nspace std
#endif

int LogLevel = DEFAULT;
char ServerName[MAXBUF];
char Network[MAXBUF];
char ServerDesc[MAXBUF];
char AdminName[MAXBUF];
char AdminEmail[MAXBUF];
char AdminNick[MAXBUF];
char diepass[MAXBUF];
char restartpass[MAXBUF];
char motd[MAXBUF];
char rules[MAXBUF];
char list[MAXBUF];
char PrefixQuit[MAXBUF];
char DieValue[MAXBUF];
char DNSServer[MAXBUF];
int debugging =  0;
int WHOWAS_STALE = 48; // default WHOWAS Entries last 2 days before they go 'stale'
int WHOWAS_MAX = 100;  // default 100 people maximum in the WHOWAS list
int DieDelay  =  5;
time_t startup_time = time(NULL);
int NetBufferSize = 10240; // NetBufferSize used as the buffer size for all read() ops
extern int MaxWhoResults;
time_t nb_start = 0;
int dns_timeout = 5;

char DisabledCommands[MAXBUF];

bool AllowHalfop = true;
bool AllowProtect = true;
bool AllowFounder = true;

extern std::vector<Module*> modules;
std::vector<std::string> module_names;
extern std::vector<ircd_module*> factory;

extern int MODCOUNT;
int openSockfd[MAXSOCKS];
bool nofork = false;
bool unlimitcore = false;

time_t TIME = time(NULL), OLDTIME = time(NULL);

#ifdef USE_KQUEUE
int kq, lkq, skq;
#endif

namespace nspace
{
#ifdef GCC34
        template<> struct hash<in_addr>
#else
        template<> struct nspace::hash<in_addr>
#endif
        {
                size_t operator()(const struct in_addr &a) const
                {
                        size_t q;
                        memcpy(&q,&a,sizeof(size_t));
                        return q;
                }
        };
#ifdef GCC34
        template<> struct hash<string>
#else
        template<> struct nspace::hash<string>
#endif
        {
                size_t operator()(const string &s) const
                {
                        char a[MAXBUF];
                        static struct hash<const char *> strhash;
                        strlcpy(a,s.c_str(),MAXBUF);
                        strlower(a);
                        return strhash(a);
                }
        };
}


struct StrHashComp
{

	bool operator()(const string& s1, const string& s2) const
	{
		char a[MAXBUF],b[MAXBUF];
		strlcpy(a,s1.c_str(),MAXBUF);
		strlcpy(b,s2.c_str(),MAXBUF);
		strlower(a);
		strlower(b);
		return (strcasecmp(a,b) == 0);
	}

};

struct InAddr_HashComp
{

	bool operator()(const in_addr &s1, const in_addr &s2) const
	{
		size_t q;
		size_t p;
		
		memcpy(&q,&s1,sizeof(size_t));
		memcpy(&p,&s2,sizeof(size_t));
		
		return (q == p);
	}

};


typedef nspace::hash_map<std::string, userrec*, nspace::hash<string>, StrHashComp> user_hash;
typedef nspace::hash_map<std::string, chanrec*, nspace::hash<string>, StrHashComp> chan_hash;
typedef nspace::hash_map<in_addr,string*, nspace::hash<in_addr>, InAddr_HashComp> address_cache;
typedef std::deque<command_t> command_table;

// This table references users by file descriptor.
// its an array to make it VERY fast, as all lookups are referenced
// by an integer, meaning there is no need for a scan/search operation.
userrec* fd_ref_table[65536];

int statsAccept = 0, statsRefused = 0, statsUnknown = 0, statsCollisions = 0, statsDns = 0, statsDnsGood = 0, statsDnsBad = 0, statsConnects = 0, statsSent= 0, statsRecv = 0;

serverrec* me[32];

FILE *log_file;

user_hash clientlist;
chan_hash chanlist;
user_hash whowas;
command_table cmdlist;
file_cache MOTD;
file_cache RULES;
address_cache IP;

ClassVector Classes;

struct linger linger = { 0 };
char MyExecutable[1024];
int boundPortCount = 0;
int portCount = 0, SERVERportCount = 0, ports[MAXSOCKS];
int defaultRoute = 0;
char ModPath[MAXBUF];

/* prototypes */

int has_channel(userrec *u, chanrec *c);
int usercount(chanrec *c);
int usercount_i(chanrec *c);
char* Passwd(userrec *user);
bool IsDenied(userrec *user);
void AddWhoWas(userrec* u);

std::vector<long> auth_cookies;
std::stringstream config_f(stringstream::in | stringstream::out);

std::vector<userrec*> all_opers;

static char already_sent[65536];

char lowermap[255];

void AddOper(userrec* user)
{
	log(DEBUG,"Oper added to optimization list");
	all_opers.push_back(user);
}

void DeleteOper(userrec* user)
{
        for (std::vector<userrec*>::iterator a = all_opers.begin(); a < all_opers.end(); a++)
        {
                if (*a == user)
                {
                        log(DEBUG,"Oper removed from optimization list");
                        all_opers.erase(a);
                        return;
                }
        }
}

long GetRevision()
{
	char Revision[] = "$Revision$";
	char *s1 = Revision;
	char *savept;
	char *v2 = strtok_r(s1," ",&savept);
	s1 = savept;
	v2 = strtok_r(s1," ",&savept);
	s1 = savept;
	return (long)(atof(v2)*10000);
}


std::string getservername()
{
	return ServerName;
}

std::string getserverdesc()
{
	return ServerDesc;
}

std::string getnetworkname()
{
	return Network;
}

std::string getadminname()
{
	return AdminName;
}

std::string getadminemail()
{
	return AdminEmail;
}

std::string getadminnick()
{
	return AdminNick;
}

void log(int level,char *text, ...)
{
	char textbuffer[MAXBUF];
	va_list argsPtr;
	time_t rawtime;
	struct tm * timeinfo;
	if (level < LogLevel)
		return;

	time(&rawtime);
	timeinfo = localtime (&rawtime);

	if (log_file)
	{
		char b[MAXBUF];
		va_start (argsPtr, text);
		vsnprintf(textbuffer, MAXBUF, text, argsPtr);
		va_end(argsPtr);
		strlcpy(b,asctime(timeinfo),MAXBUF);
		b[24] = ':';	// we know this is the end of the time string
		fprintf(log_file,"%s %s\n",b,textbuffer);
		if (nofork)
		{
			// nofork enabled? display it on terminal too
			printf("%s %s\n",b,textbuffer);
		}
	}
}

void readfile(file_cache &F, const char* fname)
{
	FILE* file;
	char linebuf[MAXBUF];
	
	log(DEBUG,"readfile: loading %s",fname);
	F.clear();
	file =  fopen(fname,"r");
	if (file)
	{
		while (!feof(file))
		{
			fgets(linebuf,sizeof(linebuf),file);
			linebuf[strlen(linebuf)-1]='\0';
			if (linebuf[0] == 0)
			{
				strcpy(linebuf,"  ");
			}
			if (!feof(file))
			{
				F.push_back(linebuf);
			}
		}
		fclose(file);
	}
	else
	{
		log(DEBUG,"readfile: failed to load file: %s",fname);
	}
	log(DEBUG,"readfile: loaded %s, %lu lines",fname,(unsigned long)F.size());
}

void ReadConfig(bool bail, userrec* user)
{
	char dbg[MAXBUF],pauseval[MAXBUF],Value[MAXBUF],timeout[MAXBUF],NB[MAXBUF],flood[MAXBUF],MW[MAXBUF];
	char AH[MAXBUF],AP[MAXBUF],AF[MAXBUF],DNT[MAXBUF],pfreq[MAXBUF],thold[MAXBUF];
	ConnectClass c;
	std::stringstream errstr;
	
	if (!LoadConf(CONFIG_FILE,&config_f,&errstr))
	{
		errstr.seekg(0);
		if (bail)
		{
			printf("There were errors in your configuration:\n%s",errstr.str().c_str());
			Exit(0);
		}
		else
		{
			char dataline[1024];
			if (user)
			{
				WriteServ(user->fd,"NOTICE %s :There were errors in the configuration file:",user->nick);
				while (!errstr.eof())
				{
					errstr.getline(dataline,1024);
					WriteServ(user->fd,"NOTICE %s :%s",user->nick,dataline);
				}
			}
			else
			{
				WriteOpers("There were errors in the configuration file:",user->nick);
				while (!errstr.eof())
				{
					errstr.getline(dataline,1024);
					WriteOpers(dataline);
				}
			}
			return;
		}
	}
	  
	ConfValue("server","name",0,ServerName,&config_f);
	ConfValue("server","description",0,ServerDesc,&config_f);
	ConfValue("server","network",0,Network,&config_f);
	ConfValue("admin","name",0,AdminName,&config_f);
	ConfValue("admin","email",0,AdminEmail,&config_f);
	ConfValue("admin","nick",0,AdminNick,&config_f);
	ConfValue("files","motd",0,motd,&config_f);
	ConfValue("files","rules",0,rules,&config_f);
	ConfValue("power","diepass",0,diepass,&config_f);
	ConfValue("power","pause",0,pauseval,&config_f);
	ConfValue("power","restartpass",0,restartpass,&config_f);
	ConfValue("options","prefixquit",0,PrefixQuit,&config_f);
	ConfValue("die","value",0,DieValue,&config_f);
	ConfValue("options","loglevel",0,dbg,&config_f);
	ConfValue("options","netbuffersize",0,NB,&config_f);
	ConfValue("options","maxwho",0,MW,&config_f);
	ConfValue("options","allowhalfop",0,AH,&config_f);
	ConfValue("options","allowprotect",0,AP,&config_f);
	ConfValue("options","allowfounder",0,AF,&config_f);
	ConfValue("dns","server",0,DNSServer,&config_f);
	ConfValue("dns","timeout",0,DNT,&config_f);
	ConfValue("options","moduledir",0,ModPath,&config_f);
        ConfValue("disabled","commands",0,DisabledCommands,&config_f);

	NetBufferSize = atoi(NB);
	MaxWhoResults = atoi(MW);
	dns_timeout = atoi(DNT);
	if (!dns_timeout)
		dns_timeout = 5;
	if (!DNSServer[0])
		strlcpy(DNSServer,"127.0.0.1",MAXBUF);
	if (!ModPath[0])
		strlcpy(ModPath,MOD_PATH,MAXBUF);
	AllowHalfop = ((!strcasecmp(AH,"true")) || (!strcasecmp(AH,"1")) || (!strcasecmp(AH,"yes")));
	AllowProtect = ((!strcasecmp(AP,"true")) || (!strcasecmp(AP,"1")) || (!strcasecmp(AP,"yes")));
	AllowFounder = ((!strcasecmp(AF,"true")) || (!strcasecmp(AF,"1")) || (!strcasecmp(AF,"yes")));
	if ((!NetBufferSize) || (NetBufferSize > 65535) || (NetBufferSize < 1024))
	{
		log(DEFAULT,"No NetBufferSize specified or size out of range, setting to default of 10240.");
		NetBufferSize = 10240;
	}
	if ((!MaxWhoResults) || (MaxWhoResults > 65535) || (MaxWhoResults < 1))
	{
		log(DEFAULT,"No MaxWhoResults specified or size out of range, setting to default of 128.");
		MaxWhoResults = 128;
	}
	if (!strcmp(dbg,"debug"))
		LogLevel = DEBUG;
	if (!strcmp(dbg,"verbose"))
		LogLevel = VERBOSE;
	if (!strcmp(dbg,"default"))
		LogLevel = DEFAULT;
	if (!strcmp(dbg,"sparse"))
		LogLevel = SPARSE;
	if (!strcmp(dbg,"none"))
		LogLevel = NONE;
	readfile(MOTD,motd);
	log(DEFAULT,"Reading message of the day...");
	readfile(RULES,rules);
	log(DEFAULT,"Reading connect classes...");
	Classes.clear();
	for (int i = 0; i < ConfValueEnum("connect",&config_f); i++)
	{
		strcpy(Value,"");
		ConfValue("connect","allow",i,Value,&config_f);
		ConfValue("connect","timeout",i,timeout,&config_f);
		ConfValue("connect","flood",i,flood,&config_f);
		ConfValue("connect","pingfreq",i,pfreq,&config_f);
		ConfValue("connect","threshold",i,thold,&config_f);
		if (Value[0])
		{
			strlcpy(c.host,Value,MAXBUF);
			c.type = CC_ALLOW;
			strlcpy(Value,"",MAXBUF);
			ConfValue("connect","password",i,Value,&config_f);
			strlcpy(c.pass,Value,MAXBUF);
			c.registration_timeout = 90; // default is 2 minutes
			c.pingtime = 120;
			c.flood = atoi(flood);
			c.threshold = 5;
			if (atoi(thold)>0)
			{
				c.threshold = atoi(thold);
			}
			if (atoi(timeout)>0)
			{
				c.registration_timeout = atoi(timeout);
			}
			if (atoi(pfreq)>0)
			{
				c.pingtime = atoi(pfreq);
			}
			Classes.push_back(c);
			log(DEBUG,"Read connect class type ALLOW, host=%s password=%s timeout=%lu flood=%lu",c.host,c.pass,(unsigned long)c.registration_timeout,(unsigned long)c.flood);
		}
		else
		{
			ConfValue("connect","deny",i,Value,&config_f);
			strlcpy(c.host,Value,MAXBUF);
			c.type = CC_DENY;
			Classes.push_back(c);
			log(DEBUG,"Read connect class type DENY, host=%s",c.host);
		}
	
	}
	log(DEFAULT,"Reading K lines,Q lines and Z lines from config...");
	read_xline_defaults();
	log(DEFAULT,"Applying K lines, Q lines and Z lines...");
	apply_lines();
	log(DEFAULT,"Done reading configuration file, InspIRCd is now starting.");
	if (!bail)
	{
		log(DEFAULT,"Adding and removing modules due to rehash...");

		std::vector<std::string> old_module_names, new_module_names, added_modules, removed_modules;

		// store the old module names
		for (std::vector<std::string>::iterator t = module_names.begin(); t != module_names.end(); t++)
		{
			old_module_names.push_back(*t);
		}

		// get the new module names
		for (int count2 = 0; count2 < ConfValueEnum("module",&config_f); count2++)
		{
			ConfValue("module","name",count2,Value,&config_f);
			new_module_names.push_back(Value);
		}

		// now create a list of new modules that are due to be loaded
		// and a seperate list of modules which are due to be unloaded
		for (std::vector<std::string>::iterator _new = new_module_names.begin(); _new != new_module_names.end(); _new++)
		{
			bool added = true;
			for (std::vector<std::string>::iterator old = old_module_names.begin(); old != old_module_names.end(); old++)
			{
				if (*old == *_new)
					added = false;
			}
			if (added)
				added_modules.push_back(*_new);
		}
		for (std::vector<std::string>::iterator oldm = old_module_names.begin(); oldm != old_module_names.end(); oldm++)
		{
			bool removed = true;
			for (std::vector<std::string>::iterator newm = new_module_names.begin(); newm != new_module_names.end(); newm++)
			{
				if (*newm == *oldm)
					removed = false;
			}
			if (removed)
				removed_modules.push_back(*oldm);
		}
		// now we have added_modules, a vector of modules to be loaded, and removed_modules, a vector of modules
		// to be removed.
		int rem = 0, add = 0;
		if (!removed_modules.empty())
		for (std::vector<std::string>::iterator removing = removed_modules.begin(); removing != removed_modules.end(); removing++)
		{
			if (UnloadModule(removing->c_str()))
			{
				WriteOpers("*** REHASH UNLOADED MODULE: %s",removing->c_str());
				WriteServ(user->fd,"973 %s %s :Module %s successfully unloaded.",user->nick, removing->c_str(), removing->c_str());
				rem++;
			}
			else
			{
				WriteServ(user->fd,"972 %s %s :Failed to unload module %s: %s",user->nick, removing->c_str(), removing->c_str(), ModuleError());
			}
		}
		if (!added_modules.empty())
		for (std::vector<std::string>::iterator adding = added_modules.begin(); adding != added_modules.end(); adding++)
		{
			if (LoadModule(adding->c_str()))
			{
				WriteOpers("*** REHASH LOADED MODULE: %s",adding->c_str());
				WriteServ(user->fd,"975 %s %s :Module %s successfully loaded.",user->nick, adding->c_str(), adding->c_str());
				add++;
			}
			else
			{
				WriteServ(user->fd,"974 %s %s :Failed to load module %s: %s",user->nick, adding->c_str(), adding->c_str(), ModuleError());
			}
		}
		log(DEFAULT,"Successfully unloaded %lu of %lu modules and loaded %lu of %lu modules.",(unsigned long)rem,(unsigned long)removed_modules.size(),(unsigned long)add,(unsigned long)added_modules.size());
	}
}

/* write formatted text to a socket, in same format as printf */

void Write(int sock,char *text, ...)
{
	if (sock == FD_MAGIC_NUMBER)
		return;
	if (!text)
	{
		log(DEFAULT,"*** BUG *** Write was given an invalid parameter");
		return;
	}
	char textbuffer[MAXBUF];
	va_list argsPtr;
	char tb[MAXBUF];
	
	va_start (argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);
	int bytes = snprintf(tb,MAXBUF,"%s\r\n",textbuffer);
	chop(tb);
	if ((sock != -1) && (sock != FD_MAGIC_NUMBER))
	{
		int MOD_RESULT = 0;
		FOREACH_RESULT(OnRawSocketWrite(sock,tb,bytes > 512 ? 512 : bytes));
		if (!MOD_RESULT)
			write(sock,tb,bytes > 512 ? 512 : bytes);
		if (fd_ref_table[sock])
		{
			fd_ref_table[sock]->bytes_out += (bytes > 512 ? 512 : bytes);
			fd_ref_table[sock]->cmds_out++;
		}
		statsSent += (bytes > 512 ? 512 : bytes);
	}
}

/* write a server formatted numeric response to a single socket */

void WriteServ(int sock, char* text, ...)
{
        if (sock == FD_MAGIC_NUMBER)
                return;
	if (!text)
	{
		log(DEFAULT,"*** BUG *** WriteServ was given an invalid parameter");
		return;
	}
	char textbuffer[MAXBUF],tb[MAXBUF];
	va_list argsPtr;
	va_start (argsPtr, text);
	
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);
	int bytes = snprintf(tb,MAXBUF,":%s %s\r\n",ServerName,textbuffer);
	chop(tb);
	if ((sock != -1) && (sock != FD_MAGIC_NUMBER))
	{
                int MOD_RESULT = 0;
                FOREACH_RESULT(OnRawSocketWrite(sock,tb,bytes > 512 ? 512 : bytes));
                if (!MOD_RESULT)
                        write(sock,tb,bytes > 512 ? 512 : bytes);
                if (fd_ref_table[sock])
                {
                        fd_ref_table[sock]->bytes_out += (bytes > 512 ? 512 : bytes);
                        fd_ref_table[sock]->cmds_out++;
                }
		statsSent += (bytes > 512 ? 512 : bytes);
	}
}

/* write text from an originating user to originating user */

void WriteFrom(int sock, userrec *user,char* text, ...)
{
        if (sock == FD_MAGIC_NUMBER)
                return;
	if ((!text) || (!user))
	{
		log(DEFAULT,"*** BUG *** WriteFrom was given an invalid parameter");
		return;
	}
	char textbuffer[MAXBUF],tb[MAXBUF];
	va_list argsPtr;
	va_start (argsPtr, text);
	
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);
	int bytes = snprintf(tb,MAXBUF,":%s!%s@%s %s\r\n",user->nick,user->ident,user->dhost,textbuffer);
	chop(tb);
	if ((sock != -1) && (sock != FD_MAGIC_NUMBER))
	{
                int MOD_RESULT = 0;
                FOREACH_RESULT(OnRawSocketWrite(sock,tb,bytes > 512 ? 512 : bytes));
                if (!MOD_RESULT)
                        write(sock,tb,bytes > 512 ? 512 : bytes);
                if (fd_ref_table[sock])
                {
                        fd_ref_table[sock]->bytes_out += (bytes > 512 ? 512 : bytes);
                        fd_ref_table[sock]->cmds_out++;
                }
		statsSent += (bytes > 512 ? 512 : bytes);
	}
}

/* write text to an destination user from a source user (e.g. user privmsg) */

void WriteTo(userrec *source, userrec *dest,char *data, ...)
{
	if ((!dest) || (!data))
	{
		log(DEFAULT,"*** BUG *** WriteTo was given an invalid parameter");
		return;
	}
        if (dest->fd == FD_MAGIC_NUMBER)
                return;
	char textbuffer[MAXBUF],tb[MAXBUF];
	va_list argsPtr;
	va_start (argsPtr, data);
	vsnprintf(textbuffer, MAXBUF, data, argsPtr);
	va_end(argsPtr);
	chop(tb);

	// if no source given send it from the server.
	if (!source)
	{
		WriteServ(dest->fd,":%s %s",ServerName,textbuffer);
	}
	else
	{
		WriteFrom(dest->fd,source,"%s",textbuffer);
	}
}

/* write formatted text from a source user to all users on a channel
 * including the sender (NOT for privmsg, notice etc!) */

void WriteChannel(chanrec* Ptr, userrec* user, char* text, ...)
{
	if ((!Ptr) || (!user) || (!text))
	{
		log(DEFAULT,"*** BUG *** WriteChannel was given an invalid parameter");
		return;
	}
	char textbuffer[MAXBUF];
	va_list argsPtr;
	va_start (argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);

	std::vector<char*> *ulist = Ptr->GetUsers();
	for (int j = 0; j < ulist->size(); j++)
	{
		char* o = (*ulist)[j];
		userrec* otheruser = (userrec*)o;
		if (otheruser->fd != FD_MAGIC_NUMBER)
			WriteTo(user,otheruser,"%s",textbuffer);
	}
}

/* write formatted text from a source user to all users on a channel
 * including the sender (NOT for privmsg, notice etc!) doesnt send to
 * users on remote servers */

void WriteChannelLocal(chanrec* Ptr, userrec* user, char* text, ...)
{
	if ((!Ptr) || (!text))
	{
		log(DEFAULT,"*** BUG *** WriteChannel was given an invalid parameter");
		return;
	}
	char textbuffer[MAXBUF];
	va_list argsPtr;
	va_start (argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);

        std::vector<char*> *ulist = Ptr->GetUsers();
        for (int j = 0; j < ulist->size(); j++)
        {
                char* o = (*ulist)[j];
                userrec* otheruser = (userrec*)o;
                if ((otheruser->fd != FD_MAGIC_NUMBER) && (otheruser->fd != -1) && (otheruser != user))
		{
			if (!user)
			{
				WriteServ(otheruser->fd,"%s",textbuffer);
			}
			else
			{
                        	WriteTo(user,otheruser,"%s",textbuffer);
			}
		}
        }
}


void WriteChannelWithServ(char* ServName, chanrec* Ptr, char* text, ...)
{
	if ((!Ptr) || (!text))
	{
		log(DEFAULT,"*** BUG *** WriteChannelWithServ was given an invalid parameter");
		return;
	}
	char textbuffer[MAXBUF];
	va_list argsPtr;
	va_start (argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);


        std::vector<char*> *ulist = Ptr->GetUsers();
        for (int j = 0; j < ulist->size(); j++)
        {
                char* o = (*ulist)[j];
                userrec* otheruser = (userrec*)o;
                if (otheruser->fd != FD_MAGIC_NUMBER)
                        WriteServ(otheruser->fd,"%s",textbuffer);
        }
}


/* write formatted text from a source user to all users on a channel except
 * for the sender (for privmsg etc) */

void ChanExceptSender(chanrec* Ptr, userrec* user, char* text, ...)
{
	if ((!Ptr) || (!user) || (!text))
	{
		log(DEFAULT,"*** BUG *** ChanExceptSender was given an invalid parameter");
		return;
	}
	char textbuffer[MAXBUF];
	va_list argsPtr;
	va_start (argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);

        std::vector<char*> *ulist = Ptr->GetUsers();
        for (int j = 0; j < ulist->size(); j++)
        {
                char* o = (*ulist)[j];
                userrec* otheruser = (userrec*)o;
                if ((otheruser->fd != FD_MAGIC_NUMBER) && (user != otheruser))
                        WriteFrom(otheruser->fd,user,"%s",textbuffer);
        }
}


std::string GetServerDescription(char* servername)
{
	for (int j = 0; j < 32; j++)
	{
		if (me[j] != NULL)
		{
			for (int k = 0; k < me[j]->connectors.size(); k++)
			{
				if (!strcasecmp(me[j]->connectors[k].GetServerName().c_str(),servername))
				{
					return me[j]->connectors[k].GetDescription();
				}
			}
		}
		return ServerDesc; // not a remote server that can be found, it must be me.
	}
}


/* write a formatted string to all users who share at least one common
 * channel, including the source user e.g. for use in NICK */

void WriteCommon(userrec *u, char* text, ...)
{
	if (!u)
	{
		log(DEFAULT,"*** BUG *** WriteCommon was given an invalid parameter");
		return;
	}

	if (u->registered != 7) {
		log(DEFAULT,"*** BUG *** WriteCommon on an unregistered user");
		return;
	}
	
	char textbuffer[MAXBUF];
	va_list argsPtr;
	va_start (argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);

	// FIX: Stops a message going to the same person more than once
	bzero(&already_sent,65536);

	bool sent_to_at_least_one = false;

        for (int i = 0; i < MAXCHANS; i++)
        {
                if (u->chans[i].channel)
                {
                        std::vector<char*> *ulist = u->chans[i].channel->GetUsers();
                        for (int j = 0; j < ulist->size(); j++)
                        {
                                char* o = (*ulist)[j];
                                userrec* otheruser = (userrec*)o;
				if ((otheruser->fd > 0) && (!already_sent[otheruser->fd]))
				{
					already_sent[otheruser->fd] = 1;
                                	WriteFrom(otheruser->fd,u,"%s",textbuffer);
					sent_to_at_least_one = true;
				}
                        }
                }
        }
	// if the user was not in any channels, no users will receive the text. Make sure the user
	// receives their OWN message for WriteCommon
	if (!sent_to_at_least_one)
	{
		WriteFrom(u->fd,u,"%s",textbuffer);
	}
}

/* write a formatted string to all users who share at least one common
 * channel, NOT including the source user e.g. for use in QUIT */

void WriteCommonExcept(userrec *u, char* text, ...)
{
	if (!u)
	{
		log(DEFAULT,"*** BUG *** WriteCommon was given an invalid parameter");
		return;
	}

	if (u->registered != 7) {
		log(DEFAULT,"*** BUG *** WriteCommon on an unregistered user");
		return;
	}

	char textbuffer[MAXBUF];
	va_list argsPtr;
	va_start (argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);

        bzero(&already_sent,65536);

        for (int i = 0; i < MAXCHANS; i++)
        {
                if (u->chans[i].channel)
                {
                        std::vector<char*> *ulist = u->chans[i].channel->GetUsers();
                        for (int j = 0; j < ulist->size(); j++)
                        {
                                char* o = (*ulist)[j];
                                userrec* otheruser = (userrec*)o;
				if (u != otheruser)
				{
                                	if ((otheruser->fd > 0) && (!already_sent[otheruser->fd]))
	                                {
        	                                already_sent[otheruser->fd] = 1;
		                                WriteFrom(otheruser->fd,u,"%s",textbuffer);
					}
				}
                        }
                }
        }
}

void WriteOpers(char* text, ...)
{
	if (!text)
	{
		log(DEFAULT,"*** BUG *** WriteOpers was given an invalid parameter");
		return;
	}

	char textbuffer[MAXBUF];
	va_list argsPtr;
	va_start (argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);

	for (std::vector<userrec*>::iterator i = all_opers.begin(); i != all_opers.end(); i++)
	{
		userrec* a = *i;
		if ((a) && (a->fd != FD_MAGIC_NUMBER))
		{
			if (strchr(a->modes,'s'))
			{
				// send server notices to all with +s
				WriteServ(a->fd,"NOTICE %s :%s",a->nick,textbuffer);
			}
		}
	}
}

void NoticeAllOpers(userrec *source, bool local_only, char* text, ...)
{
        if ((!text) || (!source))
        {
                log(DEFAULT,"*** BUG *** NoticeAllOpers was given an invalid parameter");
                return;
        }

        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);

        for (std::vector<userrec*>::iterator i = all_opers.begin(); i != all_opers.end(); i++)
        {
                userrec* a = *i;
                if ((a) && (a->fd != FD_MAGIC_NUMBER))
                {
                        if (strchr(a->modes,'s'))
                        {
                                // send server notices to all with +s
                                WriteServ(a->fd,"NOTICE %s :*** Notice From %s: %s",a->nick,source->nick,textbuffer);
                        }
                }
        }

        if (!local_only)
        {
                char buffer[MAXBUF];
                snprintf(buffer,MAXBUF,"V %s @* :%s",source->nick,textbuffer);
                NetSendToAll(buffer);
        }
}

// returns TRUE of any users on channel C occupy server 'servername'.

bool ChanAnyOnThisServer(chanrec *c,char* servername)
{
	log(DEBUG,"ChanAnyOnThisServer");

        std::vector<char*> *ulist = c->GetUsers();
        for (int j = 0; j < ulist->size(); j++)
        {
                char* o = (*ulist)[j];
                userrec* user = (userrec*)o;
                if (!strcasecmp(user->server,servername))
			return true;
        }
	return false;
}

// returns true if user 'u' shares any common channels with any users on server 'servername'

bool CommonOnThisServer(userrec* u,const char* servername)
{
	log(DEBUG,"ChanAnyOnThisServer");

        for (int i = 0; i < MAXCHANS; i++)
        {
                if (u->chans[i].channel)
                {
                        std::vector<char*> *ulist = u->chans[i].channel->GetUsers();
                        for (int j = 0; j < ulist->size(); j++)
                        {
                                char* o = (*ulist)[j];
                                userrec* user = (userrec*)o;
                                if (!strcasecmp(user->server,servername))
					return true;
                        }
                }
        }
	return false;
}


void NetSendToCommon(userrec* u, char* s)
{
	char buffer[MAXBUF];
	snprintf(buffer,MAXBUF,"%s",s);
	
	log(DEBUG,"NetSendToCommon: '%s' '%s'",u->nick,s);

        std::string msg = buffer;
        FOREACH_MOD OnPacketTransmit(msg,s);
        strlcpy(buffer,msg.c_str(),MAXBUF);

	for (int j = 0; j < 32; j++)
	{
		if (me[j] != NULL)
		{
			for (int k = 0; k < me[j]->connectors.size(); k++)
			{
				if (CommonOnThisServer(u,me[j]->connectors[k].GetServerName().c_str()))
				{
					me[j]->SendPacket(buffer,me[j]->connectors[k].GetServerName().c_str());
				}
			}
		}
	}
}


void NetSendToAll(char* s)
{
	char buffer[MAXBUF];
	snprintf(buffer,MAXBUF,"%s",s);
	
	log(DEBUG,"NetSendToAll: '%s'",s);

        std::string msg = buffer;
        FOREACH_MOD OnPacketTransmit(msg,s);
        strlcpy(buffer,msg.c_str(),MAXBUF);

	for (int j = 0; j < 32; j++)
	{
		if (me[j] != NULL)
		{
			for (int k = 0; k < me[j]->connectors.size(); k++)
			{
				me[j]->SendPacket(buffer,me[j]->connectors[k].GetServerName().c_str());
			}
		}
	}
}

void NetSendToAllAlive(char* s)
{
	char buffer[MAXBUF];
	snprintf(buffer,MAXBUF,"%s",s);
	
	log(DEBUG,"NetSendToAllAlive: '%s'",s);

        std::string msg = buffer;
        FOREACH_MOD OnPacketTransmit(msg,s);
        strlcpy(buffer,msg.c_str(),MAXBUF);

	for (int j = 0; j < 32; j++)
	{
		if (me[j] != NULL)
		{
			for (int k = 0; k < me[j]->connectors.size(); k++)
			{
				if (me[j]->connectors[k].GetState() != STATE_DISCONNECTED)
				{
					me[j]->SendPacket(buffer,me[j]->connectors[k].GetServerName().c_str());
				}
				else
				{
					log(DEBUG,"%s is dead, not sending to it.",me[j]->connectors[k].GetServerName().c_str());
				}
			}
		}
	}
}


void NetSendToOne(char* target,char* s)
{
	char buffer[MAXBUF];
	snprintf(buffer,MAXBUF,"%s",s);
	
	log(DEBUG,"NetSendToOne: '%s' '%s'",target,s);

        std::string msg = buffer;
        FOREACH_MOD OnPacketTransmit(msg,s);
        strlcpy(buffer,msg.c_str(),MAXBUF);

	for (int j = 0; j < 32; j++)
	{
		if (me[j] != NULL)
		{
			for (int k = 0; k < me[j]->connectors.size(); k++)
			{
				if (!strcasecmp(me[j]->connectors[k].GetServerName().c_str(),target))
				{
					me[j]->SendPacket(buffer,me[j]->connectors[k].GetServerName().c_str());
				}
			}
		}
	}
}

void NetSendToAllExcept(const char* target,char* s)
{
	char buffer[MAXBUF];
	snprintf(buffer,MAXBUF,"%s",s);
	
	log(DEBUG,"NetSendToAllExcept: '%s' '%s'",target,s);
	
        std::string msg = buffer;
        FOREACH_MOD OnPacketTransmit(msg,s);
        strlcpy(buffer,msg.c_str(),MAXBUF);

	for (int j = 0; j < 32; j++)
	{
		if (me[j] != NULL)
		{
			for (int k = 0; k < me[j]->connectors.size(); k++)
			{
				if (strcasecmp(me[j]->connectors[k].GetServerName().c_str(),target))
				{
					me[j]->SendPacket(buffer,me[j]->connectors[k].GetServerName().c_str());
				}
			}
		}
	}
}


void WriteMode(const char* modes, int flags, const char* text, ...)
{
	if ((!text) || (!modes) || (!flags))
	{
		log(DEFAULT,"*** BUG *** WriteMode was given an invalid parameter");
		return;
	}

	char textbuffer[MAXBUF];
	va_list argsPtr;
	va_start (argsPtr, text);
	vsnprintf(textbuffer, MAXBUF, text, argsPtr);
	va_end(argsPtr);
	int modelen = strlen(modes);

	for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
	{
		if ((i->second) && (i->second->fd != FD_MAGIC_NUMBER))
		{
			bool send_to_user = false;
			
			if (flags == WM_AND)
			{
				send_to_user = true;
				for (int n = 0; n < modelen; n++)
				{
					if (!hasumode(i->second,modes[n]))
					{
						send_to_user = false;
						break;
					}
				}
			}
			else if (flags == WM_OR)
			{
				send_to_user = false;
				for (int n = 0; n < modelen; n++)
				{
					if (hasumode(i->second,modes[n]))
					{
						send_to_user = true;
						break;
					}
				}
			}

			if (send_to_user)
			{
				WriteServ(i->second->fd,"NOTICE %s :%s",i->second->nick,textbuffer);
			}
		}
	}
}


void NoticeAll(userrec *source, bool local_only, char* text, ...)
{
        if ((!text) || (!source))
        {
                log(DEFAULT,"*** BUG *** NoticeAll was given an invalid parameter");
                return;
        }

        char textbuffer[MAXBUF];
        va_list argsPtr;
        va_start (argsPtr, text);
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);
        va_end(argsPtr);

        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if ((i->second) && (i->second->fd != FD_MAGIC_NUMBER))
                {
			WriteFrom(i->second->fd,source,"NOTICE $* :%s",textbuffer);
                }
        }

        if (!local_only)
        {
                char buffer[MAXBUF];
                snprintf(buffer,MAXBUF,"V %s * :%s",source->nick,textbuffer);
                NetSendToAll(buffer);
        }

}

void WriteWallOps(userrec *source, bool local_only, char* text, ...)  
{  
	if ((!text) || (!source))
	{
		log(DEFAULT,"*** BUG *** WriteOpers was given an invalid parameter");
		return;
	}

        char textbuffer[MAXBUF];  
        va_list argsPtr;  
        va_start (argsPtr, text);  
        vsnprintf(textbuffer, MAXBUF, text, argsPtr);  
        va_end(argsPtr);  
  
  	for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
        	if ((i->second) && (i->second->fd != FD_MAGIC_NUMBER))
        	{
                	if (strchr(i->second->modes,'w'))
                	{
				WriteTo(source,i->second,"WALLOPS :%s",textbuffer);
                	}
                }
	}

	if (!local_only)
	{
		char buffer[MAXBUF];
		snprintf(buffer,MAXBUF,"@ %s :%s",source->nick,textbuffer);
		NetSendToAll(buffer);
	}
}  

/* convert a string to lowercase. Note following special circumstances
 * taken from RFC 1459. Many "official" server branches still hold to this
 * rule so i will too;
 *
 *  Because of IRC's scandanavian origin, the characters {}| are
 *  considered to be the lower case equivalents of the characters []\,
 *  respectively. This is a critical issue when determining the
 *  equivalence of two nicknames.
 */

void strlower(char *n)
{
	if (n)
	{
		for (char* t = n; *t; t++)
			*t = lowermap[*t];
	}
}



/* Find a user record by nickname and return a pointer to it */

userrec* Find(std::string nick)
{
	user_hash::iterator iter = clientlist.find(nick);

	if (iter == clientlist.end())
		/* Couldn't find it */
		return NULL;

	return iter->second;
}

/* find a channel record by channel name and return a pointer to it */

chanrec* FindChan(const char* chan)
{
	if (!chan)
	{
		log(DEFAULT,"*** BUG *** Findchan was given an invalid parameter");
		return NULL;
	}

	chan_hash::iterator iter = chanlist.find(chan);

	if (iter == chanlist.end())
		/* Couldn't find it */
		return NULL;

	return iter->second;
}


long GetMaxBans(char* name)
{
	char CM[MAXBUF];
	for (int count = 0; count < ConfValueEnum("banlist",&config_f); count++)
	{
		ConfValue("banlist","chan",count,CM,&config_f);
		if (match(name,CM))
		{
			ConfValue("banlist","limit",count,CM,&config_f);
			return atoi(CM);
		}
	}
	return 64;
}


void purge_empty_chans(userrec* u)
{

	int go_again = 1, purge = 0;

	// firstly decrement the count on each channel
	for (int f = 0; f < MAXCHANS; f++)
	{
		if (u->chans[f].channel)
		{
			u->chans[f].channel->DecUserCounter();
			u->chans[f].channel->DelUser((char*)u);
		}
	}

	for (int i = 0; i < MAXCHANS; i++)
	{
		if (u->chans[i].channel)
		{
			if (!usercount(u->chans[i].channel))
			{
				chan_hash::iterator i2 = chanlist.find(u->chans[i].channel->name);
				/* kill the record */
				if (i2 != chanlist.end())
				{
					log(DEBUG,"del_channel: destroyed: %s",i2->second->name);
					if (i2->second)
						delete i2->second;
					chanlist.erase(i2);
					go_again = 1;
					purge++;
					u->chans[i].channel = NULL;
				}
			}
			else
			{
				log(DEBUG,"skipped purge for %s",u->chans[i].channel->name);
			}
		}
	}
	log(DEBUG,"completed channel purge, killed %lu",(unsigned long)purge);

	DeleteOper(u);
}


char scratch[MAXBUF];
char sparam[MAXBUF];

char* chanmodes(chanrec *chan)
{
	if (!chan)
	{
		log(DEFAULT,"*** BUG *** chanmodes was given an invalid parameter");
		strcpy(scratch,"");
		return scratch;
	}

	strcpy(scratch,"");
	strcpy(sparam,"");
	if (chan->noexternal)
	{
		strlcat(scratch,"n",MAXMODES);
	}
	if (chan->topiclock)
	{
		strlcat(scratch,"t",MAXMODES);
	}
	if (chan->key[0])
	{
		strlcat(scratch,"k",MAXMODES);
	}
	if (chan->limit)
	{
		strlcat(scratch,"l",MAXMODES);
	}
	if (chan->inviteonly)
	{
		strlcat(scratch,"i",MAXMODES);
	}
	if (chan->moderated)
	{
		strlcat(scratch,"m",MAXMODES);
	}
	if (chan->secret)
	{
		strlcat(scratch,"s",MAXMODES);
	}
	if (chan->c_private)
	{
		strlcat(scratch,"p",MAXMODES);
	}
	if (chan->key[0])
	{
		strlcat(sparam," ",MAXBUF);
		strlcat(sparam,chan->key,MAXBUF);
	}
	if (chan->limit)
	{
		char foo[24];
		sprintf(foo," %lu",(unsigned long)chan->limit);
		strlcat(sparam,foo,MAXBUF);
	}
	if (*chan->custom_modes)
	{
		strlcat(scratch,chan->custom_modes,MAXMODES);
		for (int z = 0; chan->custom_modes[z] != 0; z++)
		{
			std::string extparam = chan->GetModeParameter(chan->custom_modes[z]);
			if (extparam != "")
			{
				strlcat(sparam," ",MAXBUF);
				strlcat(sparam,extparam.c_str(),MAXBUF);
			}
		}
	}
	log(DEBUG,"chanmodes: %s %s%s",chan->name,scratch,sparam);
	strlcat(scratch,sparam,MAXMODES);
	return scratch;
}


/* compile a userlist of a channel into a string, each nick seperated by
 * spaces and op, voice etc status shown as @ and + */

void userlist(userrec *user,chanrec *c)
{
	if ((!c) || (!user))
	{
		log(DEFAULT,"*** BUG *** userlist was given an invalid parameter");
		return;
	}

	snprintf(list,MAXBUF,"353 %s = %s :", user->nick, c->name);

        std::vector<char*> *ulist = c->GetUsers();
  	for (int i = 0; i < ulist->size(); i++)
	{
		char* o = (*ulist)[i];
		userrec* otheruser = (userrec*)o;
		if ((!has_channel(user,c)) && (strchr(otheruser->modes,'i')))
		{
			/* user is +i, and source not on the channel, does not show
			 * nick in NAMES list */
			continue;
		}
		strlcat(list,cmode(otheruser,c),MAXBUF);
		strlcat(list,otheruser->nick,MAXBUF);
		strlcat(list," ",MAXBUF);
		if (strlen(list)>(480-NICKMAX))
		{
			/* list overflowed into
			 * multiple numerics */
			WriteServ(user->fd,"%s",list);
			snprintf(list,MAXBUF,"353 %s = %s :", user->nick, c->name);
		}
	}
	/* if whats left in the list isnt empty, send it */
	if (list[strlen(list)-1] != ':')
	{
		WriteServ(user->fd,"%s",list);
	}
}

/* return a count of the users on a specific channel accounting for
 * invisible users who won't increase the count. e.g. for /LIST */

int usercount_i(chanrec *c)
{
	int count = 0;
	
	if (!c)
	{
		log(DEFAULT,"*** BUG *** usercount_i was given an invalid parameter");
		return 0;
	}

	strcpy(list,"");
  	for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
	{
		if (i->second)
		{
			if (has_channel(i->second,c))
			{
				if (isnick(i->second->nick))
				{
					if ((!has_channel(i->second,c)) && (strchr(i->second->modes,'i')))
					{
						/* user is +i, and source not on the channel, does not show
						 * nick in NAMES list */
						continue;
					}
					count++;
				}
			}
		}
	}
	log(DEBUG,"usercount_i: %s %lu",c->name,(unsigned long)count);
	return count;
}


int usercount(chanrec *c)
{
	if (!c)
	{
		log(DEFAULT,"*** BUG *** usercount was given an invalid parameter");
		return 0;
	}
	int count = c->GetUserCounter();
	log(DEBUG,"usercount: %s %lu",c->name,(unsigned long)count);
	return count;
}


/* add a channel to a user, creating the record for it if needed and linking
 * it to the user record */

chanrec* add_channel(userrec *user, const char* cn, const char* key, bool override)
{
	if ((!user) || (!cn))
	{
		log(DEFAULT,"*** BUG *** add_channel was given an invalid parameter");
		return 0;
	}

	chanrec* Ptr;
	int created = 0;
	char cname[MAXBUF];

	strncpy(cname,cn,MAXBUF);
	
	// we MUST declare this wherever we use FOREACH_RESULT
	int MOD_RESULT = 0;

	if (strlen(cname) > CHANMAX-1)
	{
		cname[CHANMAX-1] = '\0';
	}

	log(DEBUG,"add_channel: %s %s",user->nick,cname);
	
	if ((FindChan(cname)) && (has_channel(user,FindChan(cname))))
	{
		return NULL; // already on the channel!
	}


	if (!FindChan(cname))
	{
		MOD_RESULT = 0;
		FOREACH_RESULT(OnUserPreJoin(user,NULL,cname));
		if (MOD_RESULT == 1) {
			return NULL;
		}

		/* create a new one */
		log(DEBUG,"add_channel: creating: %s",cname);
		{
			chanlist[cname] = new chanrec();

			strlcpy(chanlist[cname]->name, cname,CHANMAX);
			chanlist[cname]->topiclock = 1;
			chanlist[cname]->noexternal = 1;
			chanlist[cname]->created = TIME;
			strcpy(chanlist[cname]->topic, "");
			strncpy(chanlist[cname]->setby, user->nick,NICKMAX);
			chanlist[cname]->topicset = 0;
			Ptr = chanlist[cname];
			log(DEBUG,"add_channel: created: %s",cname);
			/* set created to 2 to indicate user
			 * is the first in the channel
			 * and should be given ops */
			created = 2;
		}
	}
	else
	{
		/* channel exists, just fish out a pointer to its struct */
		Ptr = FindChan(cname);
		if (Ptr)
		{
			log(DEBUG,"add_channel: joining to: %s",Ptr->name);
			
			// the override flag allows us to bypass channel modes
			// and bans (used by servers)
			if ((!override) || (!strcasecmp(user->server,ServerName)))
			{
				log(DEBUG,"Not overriding...");
				MOD_RESULT = 0;
				FOREACH_RESULT(OnUserPreJoin(user,Ptr,cname));
				if (MOD_RESULT == 1) {
					return NULL;
				}
				log(DEBUG,"MOD_RESULT=%d",MOD_RESULT);
				
				if (!MOD_RESULT) 
				{
					log(DEBUG,"add_channel: checking key, invite, etc");
					MOD_RESULT = 0;
					FOREACH_RESULT(OnCheckKey(user, Ptr, key ? key : ""));
					if (MOD_RESULT == 0)
					{
						if (Ptr->key[0])
						{
							log(DEBUG,"add_channel: %s has key %s",Ptr->name,Ptr->key);
							if (!key)
							{
								log(DEBUG,"add_channel: no key given in JOIN");
								WriteServ(user->fd,"475 %s %s :Cannot join channel (Requires key)",user->nick, Ptr->name);
								return NULL;
							}
							else
							{
								if (strcasecmp(key,Ptr->key))
								{
									log(DEBUG,"add_channel: bad key given in JOIN");
									WriteServ(user->fd,"475 %s %s :Cannot join channel (Incorrect key)",user->nick, Ptr->name);
									return NULL;
								}
							}
						}
						log(DEBUG,"add_channel: no key");
					}
					MOD_RESULT = 0;
					FOREACH_RESULT(OnCheckInvite(user, Ptr));
					if (MOD_RESULT == 0)
					{
						if (Ptr->inviteonly)
						{
							log(DEBUG,"add_channel: channel is +i");
							if (user->IsInvited(Ptr->name))
							{
								/* user was invited to channel */
								/* there may be an optional channel NOTICE here */
							}
							else
							{
								WriteServ(user->fd,"473 %s %s :Cannot join channel (Invite only)",user->nick, Ptr->name);
								return NULL;
							}
						}
						log(DEBUG,"add_channel: channel is not +i");
					}
					MOD_RESULT = 0;
					FOREACH_RESULT(OnCheckLimit(user, Ptr));
					if (MOD_RESULT == 0)
					{
						if (Ptr->limit)
						{
							if (usercount(Ptr) >= Ptr->limit)
							{
								WriteServ(user->fd,"471 %s %s :Cannot join channel (Channel is full)",user->nick, Ptr->name);
								return NULL;
							}
						}
					}
					log(DEBUG,"add_channel: about to walk banlist");
					MOD_RESULT = 0;
					FOREACH_RESULT(OnCheckBan(user, Ptr));
					if (MOD_RESULT == 0)
					{
						/* check user against the channel banlist */
						if (Ptr)
						{
							if (Ptr->bans.size())
							{
								for (BanList::iterator i = Ptr->bans.begin(); i != Ptr->bans.end(); i++)
								{
									if (match(user->GetFullHost(),i->data))
									{
										WriteServ(user->fd,"474 %s %s :Cannot join channel (You're banned)",user->nick, Ptr->name);
										return NULL;
									}
								}
							}
						}
						log(DEBUG,"add_channel: bans checked");
					}
				
				}
				

				if ((Ptr) && (user))
				{
					user->RemoveInvite(Ptr->name);
				}
	
				log(DEBUG,"add_channel: invites removed");

			}
			else
			{
				log(DEBUG,"Overridden checks");
			}

			
		}
		created = 1;
	}

	log(DEBUG,"Passed channel checks");
	
	for (int index =0; index != MAXCHANS; index++)
	{
		log(DEBUG,"Check location %d",index);
		if (user->chans[index].channel == NULL)
		{
			log(DEBUG,"Adding into their channel list at location %d",index);

			if (created == 2) 
			{
				/* first user in is given ops */
				user->chans[index].uc_modes = UCMODE_OP;
			}
			else
			{
				user->chans[index].uc_modes = 0;
			}
			user->chans[index].channel = Ptr;
			Ptr->IncUserCounter();
			Ptr->AddUser((char*)user);
			WriteChannel(Ptr,user,"JOIN :%s",Ptr->name);
			
			if (!override) // we're not overriding... so this isnt part of a netburst, broadcast it.
			{
				// use the stamdard J token with no privilages.
				char buffer[MAXBUF];
				if (created == 2)
				{
					snprintf(buffer,MAXBUF,"J %s @%s",user->nick,Ptr->name);
				}
				else
				{
					snprintf(buffer,MAXBUF,"J %s %s",user->nick,Ptr->name);
				}
				NetSendToAll(buffer);
			}

			log(DEBUG,"Sent JOIN to client");

			if (Ptr->topicset)
			{
				WriteServ(user->fd,"332 %s %s :%s", user->nick, Ptr->name, Ptr->topic);
				WriteServ(user->fd,"333 %s %s %s %lu", user->nick, Ptr->name, Ptr->setby, (unsigned long)Ptr->topicset);
			}
			userlist(user,Ptr);
			WriteServ(user->fd,"366 %s %s :End of /NAMES list.", user->nick, Ptr->name);
			//WriteServ(user->fd,"324 %s %s +%s",user->nick, Ptr->name,chanmodes(Ptr));
			//WriteServ(user->fd,"329 %s %s %lu", user->nick, Ptr->name, (unsigned long)Ptr->created);
			FOREACH_MOD OnUserJoin(user,Ptr);
			return Ptr;
		}
	}
	log(DEBUG,"add_channel: user channel max exceeded: %s %s",user->nick,cname);
	WriteServ(user->fd,"405 %s %s :You are on too many channels",user->nick, cname);
	return NULL;
}

/* remove a channel from a users record, and remove the record from memory
 * if the channel has become empty */

chanrec* del_channel(userrec *user, const char* cname, const char* reason, bool local)
{
	if ((!user) || (!cname))
	{
		log(DEFAULT,"*** BUG *** del_channel was given an invalid parameter");
		return NULL;
	}

	chanrec* Ptr;

	if ((!cname) || (!user))
	{
		return NULL;
	}

	Ptr = FindChan(cname);
	
	if (!Ptr)
	{
		return NULL;
	}

	FOREACH_MOD OnUserPart(user,Ptr);
	log(DEBUG,"del_channel: removing: %s %s",user->nick,Ptr->name);
	
	for (int i =0; i != MAXCHANS; i++)
	{
		/* zap it from the channel list of the user */
		if (user->chans[i].channel == Ptr)
		{
			if (reason)
			{
				WriteChannel(Ptr,user,"PART %s :%s",Ptr->name, reason);

				if (!local)
				{
					char buffer[MAXBUF];
					snprintf(buffer,MAXBUF,"L %s %s :%s",user->nick,Ptr->name,reason);
					NetSendToAll(buffer);
				}

				
			}
			else
			{
				if (!local)
				{
					char buffer[MAXBUF];
					snprintf(buffer,MAXBUF,"L %s %s :",user->nick,Ptr->name);
					NetSendToAll(buffer);
				}
			
				WriteChannel(Ptr,user,"PART :%s",Ptr->name);
			}
			user->chans[i].uc_modes = 0;
			user->chans[i].channel = NULL;
			log(DEBUG,"del_channel: unlinked: %s %s",user->nick,Ptr->name);
			break;
		}
	}

	Ptr->DecUserCounter();
	Ptr->DelUser((char*)user);
	
	/* if there are no users left on the channel */
	if (!usercount(Ptr))
	{
		chan_hash::iterator iter = chanlist.find(Ptr->name);

		log(DEBUG,"del_channel: destroying channel: %s",Ptr->name);

		/* kill the record */
		if (iter != chanlist.end())
		{
			log(DEBUG,"del_channel: destroyed: %s",Ptr->name);
			delete Ptr;
			chanlist.erase(iter);
		}
	}
}


void kick_channel(userrec *src,userrec *user, chanrec *Ptr, char* reason)
{
	if ((!src) || (!user) || (!Ptr) || (!reason))
	{
		log(DEFAULT,"*** BUG *** kick_channel was given an invalid parameter");
		return;
	}

	if ((!Ptr) || (!user) || (!src))
	{
		return;
	}

	log(DEBUG,"kick_channel: removing: %s %s %s",user->nick,Ptr->name,src->nick);

	if (!has_channel(user,Ptr))
	{
		WriteServ(src->fd,"441 %s %s %s :They are not on that channel",src->nick, user->nick, Ptr->name);
		return;
	}

	int MOD_RESULT = 0;
	FOREACH_RESULT(OnAccessCheck(src,user,Ptr,AC_KICK));
	if (MOD_RESULT == ACR_DENY)
		return;

	if (MOD_RESULT == ACR_DEFAULT)
	{
 		if (((cstatus(src,Ptr) < STATUS_HOP) || (cstatus(src,Ptr) < cstatus(user,Ptr))) && (!is_uline(src->server)))
		{
			if (cstatus(src,Ptr) == STATUS_HOP)
			{
				WriteServ(src->fd,"482 %s %s :You must be a channel operator",src->nick, Ptr->name);
			}
			else
			{
				WriteServ(src->fd,"482 %s %s :You must be at least a half-operator to change modes on this channel",src->nick, Ptr->name);
			}
			
			return;
		}
	}

	MOD_RESULT = 0;
	FOREACH_RESULT(OnUserPreKick(src,user,Ptr,reason));
	if (MOD_RESULT)
		return;

	FOREACH_MOD OnUserKick(src,user,Ptr,reason);

	for (int i =0; i != MAXCHANS; i++)
	{
		/* zap it from the channel list of the user */
		if (user->chans[i].channel)
		if (!strcasecmp(user->chans[i].channel->name,Ptr->name))
		{
			WriteChannel(Ptr,src,"KICK %s %s :%s",Ptr->name, user->nick, reason);
			user->chans[i].uc_modes = 0;
			user->chans[i].channel = NULL;
			log(DEBUG,"del_channel: unlinked: %s %s",user->nick,Ptr->name);
			break;
		}
	}

	Ptr->DecUserCounter();
	Ptr->DelUser((char*)user);

	/* if there are no users left on the channel */
	if (!usercount(Ptr))
	{
		chan_hash::iterator iter = chanlist.find(Ptr->name);

		log(DEBUG,"del_channel: destroying channel: %s",Ptr->name);

		/* kill the record */
		if (iter != chanlist.end())
		{
			log(DEBUG,"del_channel: destroyed: %s",Ptr->name);
			delete Ptr;
			chanlist.erase(iter);
		}
	}
}




/* This function pokes and hacks at a parameter list like the following:
 *
 * PART #winbot,#darkgalaxy :m00!
 *
 * to turn it into a series of individual calls like this:
 *
 * PART #winbot :m00!
 * PART #darkgalaxy :m00!
 *
 * The seperate calls are sent to a callback function provided by the caller
 * (the caller will usually call itself recursively). The callback function
 * must be a command handler. Calling this function on a line with no list causes
 * no action to be taken. You must provide a starting and ending parameter number
 * where the range of the list can be found, useful if you have a terminating
 * parameter as above which is actually not part of the list, or parameters
 * before the actual list as well. This code is used by many functions which
 * can function as "one to list" (see the RFC) */

int loop_call(handlerfunc fn, char **parameters, int pcnt, userrec *u, int start, int end, int joins)
{
	char plist[MAXBUF];
	char *param;
	char *pars[32];
	char blog[32][MAXBUF];
	char blog2[32][MAXBUF];
	int j = 0, q = 0, total = 0, t = 0, t2 = 0, total2 = 0;
	char keystr[MAXBUF];
	char moo[MAXBUF];

	for (int i = 0; i <32; i++)
		strcpy(blog[i],"");

	for (int i = 0; i <32; i++)
		strcpy(blog2[i],"");

	strcpy(moo,"");
	for (int i = 0; i <10; i++)
	{
		if (!parameters[i])
		{
			parameters[i] = moo;
		}
	}
	if (joins)
	{
		if (pcnt > 1) /* we have a key to copy */
		{
			strlcpy(keystr,parameters[1],MAXBUF);
		}
	}

	if (!parameters[start])
	{
		return 0;
	}
	if (!strchr(parameters[start],','))
	{
		return 0;
	}
	strcpy(plist,"");
	for (int i = start; i <= end; i++)
	{
		if (parameters[i])
		{
			strlcat(plist,parameters[i],MAXBUF);
		}
	}
	
	j = 0;
	param = plist;

	t = strlen(plist);
	for (int i = 0; i < t; i++)
	{
		if (plist[i] == ',')
		{
			plist[i] = '\0';
			strlcpy(blog[j++],param,MAXBUF);
			param = plist+i+1;
			if (j>20)
			{
				WriteServ(u->fd,"407 %s %s :Too many targets in list, message not delivered.",u->nick,blog[j-1]);
				return 1;
			}
		}
	}
	strlcpy(blog[j++],param,MAXBUF);
	total = j;

	if ((joins) && (keystr) && (total>0)) // more than one channel and is joining
	{
		strcat(keystr,",");
	}
	
	if ((joins) && (keystr))
	{
		if (strchr(keystr,','))
		{
			j = 0;
			param = keystr;
			t2 = strlen(keystr);
			for (int i = 0; i < t2; i++)
			{
				if (keystr[i] == ',')
				{
					keystr[i] = '\0';
					strlcpy(blog2[j++],param,MAXBUF);
					param = keystr+i+1;
				}
			}
			strlcpy(blog2[j++],param,MAXBUF);
			total2 = j;
		}
	}

	for (j = 0; j < total; j++)
	{
		if (blog[j])
		{
			pars[0] = blog[j];
		}
		for (q = end; q < pcnt-1; q++)
		{
			if (parameters[q+1])
			{
				pars[q-end+1] = parameters[q+1];
			}
		}
		if ((joins) && (parameters[1]))
		{
			if (pcnt > 1)
			{
				pars[1] = blog2[j];
			}
			else
			{
				pars[1] = NULL;
			}
		}
		/* repeatedly call the function with the hacked parameter list */
		if ((joins) && (pcnt > 1))
		{
			if (pars[1])
			{
				// pars[1] already set up and containing key from blog2[j]
				fn(pars,2,u);
			}
			else
			{
				pars[1] = parameters[1];
				fn(pars,2,u);
			}
		}
		else
		{
			fn(pars,pcnt-(end-start),u);
		}
	}

	return 1;
}



void kill_link(userrec *user,const char* r)
{
	user_hash::iterator iter = clientlist.find(user->nick);
	
	char reason[MAXBUF];
	
	strncpy(reason,r,MAXBUF);

	if (strlen(reason)>MAXQUIT)
	{
		reason[MAXQUIT-1] = '\0';
	}

	log(DEBUG,"kill_link: %s '%s'",user->nick,reason);
	Write(user->fd,"ERROR :Closing link (%s@%s) [%s]",user->ident,user->host,reason);
	log(DEBUG,"closing fd %lu",(unsigned long)user->fd);

	if (user->registered == 7) {
		FOREACH_MOD OnUserQuit(user);
		WriteCommonExcept(user,"QUIT :%s",reason);

		// Q token must go to ALL servers!!!
		char buffer[MAXBUF];
		snprintf(buffer,MAXBUF,"Q %s :%s",user->nick,reason);
		NetSendToAll(buffer);
	}

	FOREACH_MOD OnUserDisconnect(user);

	if (user->fd > -1)
	{
		FOREACH_MOD OnRawSocketClose(user->fd);
#ifdef USE_KQUEUE
		struct kevent ke;
		EV_SET(&ke, user->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
		int i = kevent(kq, &ke, 1, 0, 0, NULL);
		if (i == -1)
		{
			log(DEBUG,"kqueue: Failed to remove user from queue!");
		}
#endif
                shutdown(user->fd,2);
                close(user->fd);
	}
	
	if (user->registered == 7) {
		WriteOpers("*** Client exiting: %s!%s@%s [%s]",user->nick,user->ident,user->host,reason);
		AddWhoWas(user);
	}

        if (user->registered == 7) {
                purge_empty_chans(user);
        }

	if (iter != clientlist.end())
	{
		log(DEBUG,"deleting user hash value %lu",(unsigned long)user);
		if (user->fd > -1)
			fd_ref_table[user->fd] = NULL;
		delete user;
		clientlist.erase(iter);
	}
}

void kill_link_silent(userrec *user,const char* r)
{
	user_hash::iterator iter = clientlist.find(user->nick);
	
	char reason[MAXBUF];
	
	strncpy(reason,r,MAXBUF);

	if (strlen(reason)>MAXQUIT)
	{
		reason[MAXQUIT-1] = '\0';
	}

	log(DEBUG,"kill_link: %s '%s'",user->nick,reason);
	Write(user->fd,"ERROR :Closing link (%s@%s) [%s]",user->ident,user->host,reason);
	log(DEBUG,"closing fd %lu",(unsigned long)user->fd);

	if (user->registered == 7) {
		FOREACH_MOD OnUserQuit(user);
		WriteCommonExcept(user,"QUIT :%s",reason);

		// Q token must go to ALL servers!!!
		char buffer[MAXBUF];
		snprintf(buffer,MAXBUF,"Q %s :%s",user->nick,reason);
		NetSendToAll(buffer);
	}

	FOREACH_MOD OnUserDisconnect(user);

        if (user->fd > -1)
        {
		FOREACH_MOD OnRawSocketClose(user->fd);
#ifdef USE_KQUEUE
                struct kevent ke;
                EV_SET(&ke, user->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                int i = kevent(kq, &ke, 1, 0, 0, NULL);
                if (i == -1)
                {
                        log(DEBUG,"kqueue: Failed to remove user from queue!");
                }
#endif
                shutdown(user->fd,2);
                close(user->fd);
        }

        if (user->registered == 7) {
                purge_empty_chans(user);
        }
	
	if (iter != clientlist.end())
	{
		log(DEBUG,"deleting user hash value %lu",(unsigned long)user);
                if (user->fd > -1)
                        fd_ref_table[user->fd] = NULL;
		delete user;
		clientlist.erase(iter);
	}
}



// looks up a users password for their connection class (<ALLOW>/<DENY> tags)

char* Passwd(userrec *user)
{
	for (ClassVector::iterator i = Classes.begin(); i != Classes.end(); i++)
	{
		if (match(user->host,i->host) && (i->type == CC_ALLOW))
		{
			return i->pass;
		}
	}
	return "";
}

bool IsDenied(userrec *user)
{
	for (ClassVector::iterator i = Classes.begin(); i != Classes.end(); i++)
	{
		if (match(user->host,i->host) && (i->type == CC_DENY))
		{
			return true;
		}
	}
	return false;
}




/* sends out an error notice to all connected clients (not to be used
 * lightly!) */

void send_error(char *s)
{
	log(DEBUG,"send_error: %s",s);
  	for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
	{
		if (isnick(i->second->nick))
		{
			WriteServ(i->second->fd,"NOTICE %s :%s",i->second->nick,s);
		}
		else
		{
			// fix - unregistered connections receive ERROR, not NOTICE
			Write(i->second->fd,"ERROR :%s",s);
		}
	}
}

void Error(int status)
{
	signal (SIGALRM, SIG_IGN);
	signal (SIGPIPE, SIG_IGN);
	signal (SIGTERM, SIG_IGN);
	signal (SIGABRT, SIG_IGN);
	signal (SIGSEGV, SIG_IGN);
	signal (SIGURG, SIG_IGN);
	signal (SIGKILL, SIG_IGN);
	log(DEFAULT,"*** fell down a pothole in the road to perfection ***");
	send_error("Error! Segmentation fault! save meeeeeeeeeeeeee *splat!*");
	Exit(status);
}


int main(int argc, char** argv)
{
	Start();
	srand(time(NULL));
	log(DEBUG,"*** InspIRCd starting up!");
	if (!FileExists(CONFIG_FILE))
	{
		printf("ERROR: Cannot open config file: %s\nExiting...\n",CONFIG_FILE);
		log(DEFAULT,"main: no config");
		printf("ERROR: Your config file is missing, this IRCd will self destruct in 10 seconds!\n");
		Exit(ERROR);
	}
	if (argc > 1) {
		for (int i = 1; i < argc; i++)
		{
			if (!strcmp(argv[i],"-nofork")) {
				nofork = true;
			}
			if (!strcmp(argv[i],"-wait")) {
				sleep(6);
			}
			if (!strcmp(argv[i],"-nolimit")) {
				unlimitcore = true;
			}
		}
	}
	strlcpy(MyExecutable,argv[0],MAXBUF);
	
	// initialize the lowercase mapping table
	for (int cn = 0; cn < 256; cn++)
		lowermap[cn] = cn;
	// lowercase the uppercase chars
	for (int cn = 65; cn < 91; cn++)
		lowermap[cn] = tolower(cn);
	// now replace the specific chars for scandanavian comparison
	lowermap['['] = '{';
	lowermap[']'] = '}';
	lowermap['\\'] = '|';

	if (InspIRCd(argv,argc) == ERROR)
	{
		log(DEFAULT,"main: daemon function bailed");
		printf("ERROR: could not initialise. Shutting down.\n");
		Exit(ERROR);
	}
	Exit(TRUE);
	return 0;
}

template<typename T> inline string ConvToStr(const T &in)
{
	stringstream tmp;
	if (!(tmp << in)) return string();
	return tmp.str();
}

/* re-allocates a nick in the user_hash after they change nicknames,
 * returns a pointer to the new user as it may have moved */

userrec* ReHashNick(char* Old, char* New)
{
	//user_hash::iterator newnick;
	user_hash::iterator oldnick = clientlist.find(Old);

	log(DEBUG,"ReHashNick: %s %s",Old,New);
	
	if (!strcasecmp(Old,New))
	{
		log(DEBUG,"old nick is new nick, skipping");
		return oldnick->second;
	}
	
	if (oldnick == clientlist.end()) return NULL; /* doesnt exist */

	log(DEBUG,"ReHashNick: Found hashed nick %s",Old);

	clientlist[New] = new userrec();
	clientlist[New] = oldnick->second;
	clientlist.erase(oldnick);

	log(DEBUG,"ReHashNick: Nick rehashed as %s",New);
	
	return clientlist[New];
}

/* adds or updates an entry in the whowas list */
void AddWhoWas(userrec* u)
{
	user_hash::iterator iter = whowas.find(u->nick);
	userrec *a = new userrec();
	strlcpy(a->nick,u->nick,NICKMAX);
	strlcpy(a->ident,u->ident,64);
	strlcpy(a->dhost,u->dhost,256);
	strlcpy(a->host,u->host,256);
	strlcpy(a->fullname,u->fullname,128);
	strlcpy(a->server,u->server,256);
	a->signon = u->signon;

	/* MAX_WHOWAS:   max number of /WHOWAS items
	 * WHOWAS_STALE: number of hours before a WHOWAS item is marked as stale and
	 *		 can be replaced by a newer one
	 */
	
	if (iter == whowas.end())
	{
		if (whowas.size() == WHOWAS_MAX)
		{
			for (user_hash::iterator i = whowas.begin(); i != whowas.end(); i++)
			{
				// 3600 seconds in an hour ;)
				if ((i->second->signon)<(TIME-(WHOWAS_STALE*3600)))
				{
					if (i->second) delete i->second;
					i->second = a;
					log(DEBUG,"added WHOWAS entry, purged an old record");
					return;
				}
			}
		}
		else
		{
			log(DEBUG,"added fresh WHOWAS entry");
			whowas[a->nick] = a;
		}
	}
	else
	{
		log(DEBUG,"updated WHOWAS entry");
		if (iter->second) delete iter->second;
		iter->second = a;
	}
}


/* add a client connection to the sockets list */
void AddClient(int socket, char* host, int port, bool iscached, char* ip)
{
	string tempnick;
	char tn2[MAXBUF];
	user_hash::iterator iter;

	tempnick = ConvToStr(socket) + "-unknown";
	sprintf(tn2,"%lu-unknown",(unsigned long)socket);

	iter = clientlist.find(tempnick);

	// fix by brain.
	// as these nicknames are 'RFC impossible', we can be sure nobody is going to be
	// using one as a registered connection. As theyre per fd, we can also safely assume
	// that we wont have collisions. Therefore, if the nick exists in the list, its only
	// used by a dead socket, erase the iterator so that the new client may reclaim it.
	// this was probably the cause of 'server ignores me when i hammer it with reconnects'
	// issue in earlier alphas/betas
	if (iter != clientlist.end())
	{
		clientlist.erase(iter);
	}

	/*
	 * It is OK to access the value here this way since we know
	 * it exists, we just created it above.
	 *
	 * At NO other time should you access a value in a map or a
	 * hash_map this way.
	 */
	clientlist[tempnick] = new userrec();

	NonBlocking(socket);
	log(DEBUG,"AddClient: %lu %s %d %s",(unsigned long)socket,host,port,ip);

	clientlist[tempnick]->fd = socket;
	strncpy(clientlist[tempnick]->nick, tn2,NICKMAX);
	strncpy(clientlist[tempnick]->host, host,160);
	strncpy(clientlist[tempnick]->dhost, host,160);
	strncpy(clientlist[tempnick]->server, ServerName,256);
	strncpy(clientlist[tempnick]->ident, "unknown",12);
	clientlist[tempnick]->registered = 0;
	clientlist[tempnick]->signon = TIME+dns_timeout;
	clientlist[tempnick]->lastping = 1;
	clientlist[tempnick]->port = port;
	strncpy(clientlist[tempnick]->ip,ip,32);

	// set the registration timeout for this user
	unsigned long class_regtimeout = 90;
	int class_flood = 0;
	long class_threshold = 5;

	for (ClassVector::iterator i = Classes.begin(); i != Classes.end(); i++)
	{
		if (match(clientlist[tempnick]->host,i->host) && (i->type == CC_ALLOW))
		{
			class_regtimeout = (unsigned long)i->registration_timeout;
			class_flood = i->flood;
			clientlist[tempnick]->pingmax = i->pingtime;
			class_threshold = i->threshold;
			break;
		}
	}

	clientlist[tempnick]->nping = TIME+clientlist[tempnick]->pingmax+dns_timeout;
	clientlist[tempnick]->timeout = TIME+class_regtimeout;
	clientlist[tempnick]->flood = class_flood;
	clientlist[tempnick]->threshold = class_threshold;

	for (int i = 0; i < MAXCHANS; i++)
	{
 		clientlist[tempnick]->chans[i].channel = NULL;
 		clientlist[tempnick]->chans[i].uc_modes = 0;
 	}

	if (clientlist.size() == MAXCLIENTS)
	{
		kill_link(clientlist[tempnick],"No more connections allowed in this class");
		return;
	}

	// this is done as a safety check to keep the file descriptors within range of fd_ref_table.
	// its a pretty big but for the moment valid assumption:
	// file descriptors are handed out starting at 0, and are recycled as theyre freed.
	// therefore if there is ever an fd over 65535, 65536 clients must be connected to the
	// irc server at once (or the irc server otherwise initiating this many connections, files etc)
	// which for the time being is a physical impossibility (even the largest networks dont have more
	// than about 10,000 users on ONE server!)
	if (socket > 65535)
	{
		kill_link(clientlist[tempnick],"Server is full");
		return;
	}
		

        char* e = matches_exception(ip);
	if (!e)
	{
		char* r = matches_zline(ip);
		if (r)
		{
			char reason[MAXBUF];
			snprintf(reason,MAXBUF,"Z-Lined: %s",r);
			kill_link(clientlist[tempnick],reason);
			return;
		}
	}
	fd_ref_table[socket] = clientlist[tempnick];

#ifdef USE_KQUEUE
	struct kevent ke;
	log(DEBUG,"kqueue: Add user to events, kq=%d socket=%d",kq,socket);
	EV_SET(&ke, socket, EVFILT_READ, EV_ADD, 0, 0, NULL);
        int i = kevent(kq, &ke, 1, 0, 0, NULL);
        if (i == -1)
        {
		switch (errno)
		{
			case EACCES:
				log(DEBUG,"kqueue: EACCES");
			break;
			case EFAULT:
				log(DEBUG,"kqueue: EFAULT");
			break;
			case EBADF:
				log(DEBUG,"kqueue: EBADF=%d",ke.ident);
			break;
			case EINTR:
				log(DEBUG,"kqueue: EINTR");
			break;
			case EINVAL:
				log(DEBUG,"kqueue: EINVAL");
			break;
			case ENOENT:
				log(DEBUG,"kqueue: ENOENT");
			break;
			case ENOMEM:
				log(DEBUG,"kqueue: ENOMEM");
			break;
			case ESRCH:
				log(DEBUG,"kqueue: ESRCH");
			break;
			default:
				log(DEBUG,"kqueue: UNKNOWN!");
			break;
		}
                log(DEBUG,"kqueue: Failed to add user to queue!");
        }

#endif
}

// this function counts all users connected, wether they are registered or NOT.
int usercnt(void)
{
	return clientlist.size();
}

// this counts only registered users, so that the percentages in /MAP don't mess up when users are sitting in an unregistered state
int registered_usercount(void)
{
        int c = 0;
        for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
        {
                if ((i->second->fd) && (isnick(i->second->nick))) c++;
        }
        return c;
}

int usercount_invisible(void)
{
	int c = 0;

	for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
	{
		if ((i->second->fd) && (isnick(i->second->nick)) && (strchr(i->second->modes,'i'))) c++;
	}
	return c;
}

int usercount_opers(void)
{
	int c = 0;

	for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
	{
		if ((i->second->fd) && (isnick(i->second->nick)) && (strchr(i->second->modes,'o'))) c++;
	}
	return c;
}

int usercount_unknown(void)
{
	int c = 0;

	for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
	{
		if ((i->second->fd) && (i->second->registered != 7))
			c++;
	}
	return c;
}

long chancount(void)
{
	return chanlist.size();
}

long count_servs(void)
{
	int c = 0;
        for (int i = 0; i < 32; i++)
        {
                if (me[i] != NULL)
                {
                        for (vector<ircd_connector>::iterator j = me[i]->connectors.begin(); j != me[i]->connectors.end(); j++)
                        {
                                if (strcasecmp(j->GetServerName().c_str(),ServerName))
                                {
					c++;
                                }
                        }
                }
        }
	return c;
}

long servercount(void)
{
	return count_servs()+1;
}

long local_count()
{
	int c = 0;
	for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
	{
		if ((i->second->fd) && (isnick(i->second->nick)) && (!strcasecmp(i->second->server,ServerName))) c++;
	}
	return c;
}


void ShowMOTD(userrec *user)
{
	char buf[65536];
        std::string WholeMOTD = "";
        if (!MOTD.size())
        {
                WriteServ(user->fd,"422 %s :Message of the day file is missing.",user->nick);
                return;
        }
        snprintf(buf,65535,":%s 375 %s :- %s message of the day\r\n", ServerName, user->nick, ServerName);
	WholeMOTD = WholeMOTD + buf;
        for (int i = 0; i != MOTD.size(); i++)
        {
		snprintf(buf,65535,":%s 372 %s :- %s\r\n", ServerName, user->nick, MOTD[i].c_str());
                WholeMOTD = WholeMOTD + buf;
        }
	snprintf(buf,65535,":%s 376 %s :End of message of the day.\r\n", ServerName, user->nick);
        WholeMOTD = WholeMOTD + buf;
        // only one write operation
        send(user->fd,WholeMOTD.c_str(),WholeMOTD.length(),0);
	statsSent += WholeMOTD.length();
}

void ShowRULES(userrec *user)
{
	if (!RULES.size())
	{
		WriteServ(user->fd,"NOTICE %s :Rules file is missing.",user->nick);
		return;
	}
  	WriteServ(user->fd,"NOTICE %s :%s rules",user->nick,ServerName);
	for (int i = 0; i != RULES.size(); i++)
	{
				WriteServ(user->fd,"NOTICE %s :%s",user->nick,RULES[i].c_str());
	}
	WriteServ(user->fd,"NOTICE %s :End of %s rules.",user->nick,ServerName);
}

/* shows the message of the day, and any other on-logon stuff */
void FullConnectUser(userrec* user)
{
	statsConnects++;
        user->idle_lastmsg = TIME;
        log(DEBUG,"ConnectUser: %s",user->nick);

        if ((strcmp(Passwd(user),"")) && (!user->haspassed))
        {
                kill_link(user,"Invalid password");
                return;
        }
        if (IsDenied(user))
        {
                kill_link(user,"Unauthorised connection");
                return;
        }

        char match_against[MAXBUF];
        snprintf(match_against,MAXBUF,"%s@%s",user->ident,user->host);
	char* e = matches_exception(match_against);
	if (!e)
	{
        	char* r = matches_gline(match_against);
        	if (r)
        	{
                	char reason[MAXBUF];
                	snprintf(reason,MAXBUF,"G-Lined: %s",r);
                	kill_link_silent(user,reason);
                	return;
        	}
        	r = matches_kline(user->host);
        	if (r)
        	{
                	char reason[MAXBUF];
                	snprintf(reason,MAXBUF,"K-Lined: %s",r);
                	kill_link_silent(user,reason);
                	return;
	        }
	}

	// fix by brain: move this below the xline checks to prevent spurious quits going onto the net that dont belong
	user->registered = 7;

        WriteServ(user->fd,"NOTICE Auth :Welcome to \002%s\002!",Network);
        WriteServ(user->fd,"001 %s :Welcome to the %s IRC Network %s!%s@%s",user->nick,Network,user->nick,user->ident,user->host);
        WriteServ(user->fd,"002 %s :Your host is %s, running version %s",user->nick,ServerName,VERSION);
        WriteServ(user->fd,"003 %s :This server was created %s %s",user->nick,__TIME__,__DATE__);
        WriteServ(user->fd,"004 %s %s %s iowghraAsORVSxNCWqBzvdHtGI lvhopsmntikrRcaqOALQbSeKVfHGCuzN",user->nick,ServerName,VERSION);
        // the neatest way to construct the initial 005 numeric, considering the number of configure constants to go in it...
        std::stringstream v;
        v << "MESHED WALLCHOPS MODES=13 CHANTYPES=# PREFIX=(ohv)@%+ MAP SAFELIST MAXCHANNELS=" << MAXCHANS;
        v << " MAXBANS=60 NICKLEN=" << NICKMAX;
        v << " TOPICLEN=307 KICKLEN=307 MAXTARGETS=20 AWAYLEN=307 CHANMODES=ohvb,k,l,psmnti NETWORK=";
        v << Network;
        std::string data005 = v.str();
        FOREACH_MOD On005Numeric(data005);
        // anfl @ #ratbox, efnet reminded me that according to the RFC this cant contain more than 13 tokens per line...
        // so i'd better split it :)
        std::stringstream out(data005);
        std::string token = "";
        std::string line5 = "";
        int token_counter = 0;
        while (!out.eof())
        {
                out >> token;
                line5 = line5 + token + " ";
                token_counter++;
                if ((token_counter >= 13) || (out.eof() == true))
                {
                        WriteServ(user->fd,"005 %s %s:are supported by this server",user->nick,line5.c_str());
                        line5 = "";
                        token_counter = 0;
                }
        }
        ShowMOTD(user);

        char buffer[MAXBUF];
	snprintf(buffer,MAXBUF,"N %lu %s %s %s %s +%s %s %s :%s",(unsigned long)user->age,user->nick,user->host,user->dhost,user->ident,user->modes,user->ip,ServerName,user->fullname);
        NetSendToAll(buffer);

	// fix by brain: these should be AFTER the N token, so other servers know what the HELL we're on about... :)
	FOREACH_MOD OnUserConnect(user);
	WriteOpers("*** Client connecting on port %lu: %s!%s@%s [%s]",(unsigned long)user->port,user->nick,user->ident,user->host,user->ip);
}


// this returns 1 when all modules are satisfied that the user should be allowed onto the irc server
// (until this returns true, a user will block in the waiting state, waiting to connect up to the
// registration timeout maximum seconds)
bool AllModulesReportReady(userrec* user)
{
	for (int i = 0; i <= MODCOUNT; i++)
	{
		int res = modules[i]->OnCheckReady(user);
			if (!res)
				return false;
	}
	return true;
}

/* shows the message of the day, and any other on-logon stuff */
void ConnectUser(userrec *user)
{
	// dns is already done, things are fast. no need to wait for dns to complete just pass them straight on
	if ((user->dns_done) && (user->registered >= 3) && (AllModulesReportReady(user)))
	{
		FullConnectUser(user);
	}
}

std::string GetVersionString()
{
        char Revision[] = "$Revision$";
	char versiondata[MAXBUF];
        char *s1 = Revision;
        char *savept;
        char *v2 = strtok_r(s1," ",&savept);
        s1 = savept;
        v2 = strtok_r(s1," ",&savept);
        s1 = savept;
#ifdef USE_KQUEUE
	char socketengine[] = "kqueue";
#else
	char socketengine[] = "select";
#endif
	snprintf(versiondata,MAXBUF,"%s Rev. %s %s :%s (O=%lu) [SE=%s]",VERSION,v2,ServerName,SYSTEM,(unsigned long)OPTIMISATION,socketengine);
	return versiondata;
}

void handle_version(char **parameters, int pcnt, userrec *user)
{
	if (!pcnt)
	{
		WriteServ(user->fd,"351 %s :%s",user->nick,GetVersionString().c_str());
	}
	else
	{
		if (!strcmp(parameters[0],"*"))
		{
			for (int j = 0; j < 32; j++)
			{
				if (me[j] != NULL)
				{
					for (int x = 0; x < me[j]->connectors.size(); x++)
					{
						WriteServ(user->fd,"351 %s :Server %d:%d (%s): %s",user->nick,j,x,me[j]->connectors[x].GetServerName().c_str(),me[j]->connectors[x].GetVersionString().c_str());
					}
				}
			}
			return;
		}
		if (match(ServerName,parameters[0]))
		{
			WriteServ(user->fd,"351 %s :%s",user->nick,GetVersionString().c_str());
			return;
		}
		bool displayed = false, found = false;
                for (int j = 0; j < 32; j++)
                {
                        if (me[j] != NULL)
                        {
                                for (int x = 0; x < me[j]->connectors.size(); x++)
                                {
                                        if (match(me[j]->connectors[x].GetServerName().c_str(),parameters[0]))
                                        {
						found = true;
						if ((me[j]->connectors[x].GetVersionString() != "") && (!displayed))
						{
							displayed = true;
							WriteServ(user->fd,"351 %s :%s",user->nick,me[j]->connectors[x].GetVersionString().c_str());
						}
					}
				}
			}
		}
		if ((!displayed) && (found))
		{
			WriteServ(user->fd,"402 %s %s :Server %s has no version information",user->nick,parameters[0],parameters[0]);
			return;
		}
		if (!found)
		{
			WriteServ(user->fd,"402 %s %s :No such server",user->nick,parameters[0]);
		}
	}
	return;
}


// calls a handler function for a command

void call_handler(const char* commandname,char **parameters, int pcnt, userrec *user)
{
		for (int i = 0; i < cmdlist.size(); i++)
		{
			if (!strcasecmp(cmdlist[i].command,commandname))
			{
				if (cmdlist[i].handler_function)
				{
					if (pcnt>=cmdlist[i].min_params)
					{
						if (strchr(user->modes,cmdlist[i].flags_needed))
						{
							cmdlist[i].handler_function(parameters,pcnt,user);
						}
					}
				}
			}
		}
}

void DoSplitEveryone()
{
	bool go_again = true;
	while (go_again)
	{
		go_again = false;
		for (int i = 0; i < 32; i++)
		{
			if (me[i] != NULL)
			{
				for (vector<ircd_connector>::iterator j = me[i]->connectors.begin(); j != me[i]->connectors.end(); j++)
				{
					if (strcasecmp(j->GetServerName().c_str(),ServerName))
					{
						j->routes.clear();
						j->CloseConnection();
						me[i]->connectors.erase(j);
						go_again = true;
						break;
					}
				}
			}
		}
	}
	log(DEBUG,"Removed server. Will remove clients...");
	// iterate through the userlist and remove all users on this server.
	// because we're dealing with a mesh, we dont have to deal with anything
	// "down-route" from this server (nice huh)
	go_again = true;
	char reason[MAXBUF];
	while (go_again)
	{
		go_again = false;
		for (user_hash::const_iterator u = clientlist.begin(); u != clientlist.end(); u++)
		{
			if (strcasecmp(u->second->server,ServerName))
			{
				snprintf(reason,MAXBUF,"%s %s",ServerName,u->second->server);
				kill_link(u->second,reason);
				go_again = true;
				break;
			}
		}
	}
}



char islast(const char* s)
{
	char c = '`';
	for (int j = 0; j < 32; j++)
 	{
 		if (me[j] != NULL)
 		{
			for (int k = 0; k < me[j]->connectors.size(); k++)
			{
				if (strcasecmp(me[j]->connectors[k].GetServerName().c_str(),s))
  				{
  					c = '|';
				}
				if (!strcasecmp(me[j]->connectors[k].GetServerName().c_str(),s))
  				{
  					c = '`';
				}
			}
		}
	}
	return c;
}

long map_count(const char* s)
{
	int c = 0;
	for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
	{
		if ((i->second->fd) && (isnick(i->second->nick)) && (!strcasecmp(i->second->server,s))) c++;
	}
	return c;
}


void force_nickchange(userrec* user,const char* newnick)
{
	char nick[MAXBUF];
	int MOD_RESULT = 0;
	
	strcpy(nick,"");

	FOREACH_RESULT(OnUserPreNick(user,newnick));
	if (MOD_RESULT) {
		statsCollisions++;
		kill_link(user,"Nickname collision");
		return;
	}
	if (matches_qline(newnick))
	{
		statsCollisions++;
		kill_link(user,"Nickname collision");
		return;
	}
	
	if (user)
	{
		if (newnick)
		{
			strncpy(nick,newnick,MAXBUF);
		}
		if (user->registered == 7)
		{
			char* pars[1];
			pars[0] = nick;
			handle_nick(pars,1,user);
		}
	}
}
				

int process_parameters(char **command_p,char *parameters)
{
	int j = 0;
	int q = strlen(parameters);
	if (!q)
	{
		/* no parameters, command_p invalid! */
		return 0;
	}
	if (parameters[0] == ':')
	{
		command_p[0] = parameters+1;
		return 1;
	}
	if (q)
	{
		if ((strchr(parameters,' ')==NULL) || (parameters[0] == ':'))
		{
			/* only one parameter */
			command_p[0] = parameters;
			if (parameters[0] == ':')
			{
				if (strchr(parameters,' ') != NULL)
				{
					command_p[0]++;
				}
			}
			return 1;
		}
	}
	command_p[j++] = parameters;
	for (int i = 0; i <= q; i++)
	{
		if (parameters[i] == ' ')
		{
			command_p[j++] = parameters+i+1;
			parameters[i] = '\0';
			if (command_p[j-1][0] == ':')
			{
				*command_p[j-1]++; /* remove dodgy ":" */
				break;
				/* parameter like this marks end of the sequence */
			}
		}
	}
	return j; /* returns total number of items in the list */
}

void process_command(userrec *user, char* cmd)
{
	char *parameters;
	char *command;
	char *command_p[127];
	char p[MAXBUF], temp[MAXBUF];
	int j, items, cmd_found;

	for (int i = 0; i < 127; i++)
		command_p[i] = NULL;

	if (!user)
	{
		return;
	}
	if (!cmd)
	{
		return;
	}
	if (!cmd[0])
	{
		return;
	}
	
	int total_params = 0;
	if (strlen(cmd)>2)
	{
		for (int q = 0; q < strlen(cmd)-1; q++)
		{
			if ((cmd[q] == ' ') && (cmd[q+1] == ':'))
			{
				total_params++;
				// found a 'trailing', we dont count them after this.
				break;
			}
			if (cmd[q] == ' ')
				total_params++;
		}
	}

	// another phidjit bug...
	if (total_params > 126)
	{
		*(strchr(cmd,' ')) = '\0';
		WriteServ(user->fd,"421 %s %s :Too many parameters given",user->nick,cmd);
		return;
	}

	strlcpy(temp,cmd,MAXBUF);
	
	std::string tmp = cmd;
	for (int i = 0; i <= MODCOUNT; i++)
	{
		std::string oldtmp = tmp;
		modules[i]->OnServerRaw(tmp,true,user);
		if (oldtmp != tmp)
		{
			log(DEBUG,"A Module changed the input string!");
			log(DEBUG,"New string: %s",tmp.c_str());
			log(DEBUG,"Old string: %s",oldtmp.c_str());
			break;
		}
	}
  	strlcpy(cmd,tmp.c_str(),MAXBUF);
	strlcpy(temp,cmd,MAXBUF);

	if (!strchr(cmd,' '))
	{
		/* no parameters, lets skip the formalities and not chop up
		 * the string */
		log(DEBUG,"About to preprocess command with no params");
		items = 0;
		command_p[0] = NULL;
		parameters = NULL;
		for (int i = 0; i <= strlen(cmd); i++)
		{
			cmd[i] = toupper(cmd[i]);
		}
		command = cmd;
	}
	else
	{
		strcpy(cmd,"");
		j = 0;
		/* strip out extraneous linefeeds through mirc's crappy pasting (thanks Craig) */
		for (int i = 0; i < strlen(temp); i++)
		{
			if ((temp[i] != 10) && (temp[i] != 13) && (temp[i] != 0) && (temp[i] != 7))
			{
				cmd[j++] = temp[i];
				cmd[j] = 0;
			}
		}
		/* split the full string into a command plus parameters */
		parameters = p;
		strcpy(p," ");
		command = cmd;
		if (strchr(cmd,' '))
		{
			for (int i = 0; i <= strlen(cmd); i++)
			{
				/* capitalise the command ONLY, leave params intact */
				cmd[i] = toupper(cmd[i]);
				/* are we nearly there yet?! :P */
				if (cmd[i] == ' ')
				{
					command = cmd;
					parameters = cmd+i+1;
					cmd[i] = '\0';
					break;
				}
			}
		}
		else
		{
			for (int i = 0; i <= strlen(cmd); i++)
			{
				cmd[i] = toupper(cmd[i]);
			}
		}

	}
	cmd_found = 0;
	
	if (strlen(command)>MAXCOMMAND)
	{
		WriteServ(user->fd,"421 %s %s :Command too long",user->nick,command);
		return;
	}
	
	for (int x = 0; x < strlen(command); x++)
	{
		if (((command[x] < 'A') || (command[x] > 'Z')) && (command[x] != '.'))
		{
			if (((command[x] < '0') || (command[x]> '9')) && (command[x] != '-'))
			{
				if (strchr("@!\"$%^&*(){}[]_=+;:'#~,<>/?\\|`",command[x]))
				{
					statsUnknown++;
					WriteServ(user->fd,"421 %s %s :Unknown command",user->nick,command);
					return;
				}
			}
		}
	}

	for (int i = 0; i != cmdlist.size(); i++)
	{
		if (cmdlist[i].command[0])
		{
			if (strlen(command)>=(strlen(cmdlist[i].command))) if (!strncmp(command, cmdlist[i].command,MAXCOMMAND))
			{
				if (parameters)
				{
					if (parameters[0])
					{
						items = process_parameters(command_p,parameters);
					}
					else
					{
						items = 0;
						command_p[0] = NULL;
					}
				}
				else
				{
					items = 0;
					command_p[0] = NULL;
				}
				
				if (user)
				{
					/* activity resets the ping pending timer */
					user->nping = TIME + user->pingmax;
					if ((items) < cmdlist[i].min_params)
					{
					        log(DEBUG,"process_command: not enough parameters: %s %s",user->nick,command);
						WriteServ(user->fd,"461 %s %s :Not enough parameters",user->nick,command);
						return;
					}
					if ((!strchr(user->modes,cmdlist[i].flags_needed)) && (cmdlist[i].flags_needed))
					{
					        log(DEBUG,"process_command: permission denied: %s %s",user->nick,command);
						WriteServ(user->fd,"481 %s :Permission Denied- You do not have the required operator privilages",user->nick);
						cmd_found = 1;
						return;
					}
					if ((cmdlist[i].flags_needed) && (!user->HasPermission(command)))
					{
					        log(DEBUG,"process_command: permission denied: %s %s",user->nick,command);
						WriteServ(user->fd,"481 %s :Permission Denied- Oper type %s does not have access to command %s",user->nick,user->oper,command);
						cmd_found = 1;
						return;
					}
					/* if the command isnt USER, PASS, or NICK, and nick is empty,
					 * deny command! */
					if ((strncmp(command,"USER",4)) && (strncmp(command,"NICK",4)) && (strncmp(command,"PASS",4)))
					{
						if ((!isnick(user->nick)) || (user->registered != 7))
						{
						        log(DEBUG,"process_command: not registered: %s %s",user->nick,command);
							WriteServ(user->fd,"451 %s :You have not registered",command);
							return;
						}
					}
					if ((user->registered == 7) && (!strchr(user->modes,'o')))
					{
						char* mycmd;
						char* savept2;
						mycmd = strtok_r(DisabledCommands," ",&savept2);
						while (mycmd)
						{
							if (!strcasecmp(mycmd,command))
							{
								// command is disabled!
								WriteServ(user->fd,"421 %s %s :This command has been disabled.",user->nick,command);
								return;
							}
							mycmd = strtok_r(NULL," ",&savept2);
						}
        

					}
					if ((user->registered == 7) || (!strncmp(command,"USER",4)) || (!strncmp(command,"NICK",4)) || (!strncmp(command,"PASS",4)))
					{
						if (cmdlist[i].handler_function)
						{
							
							/* ikky /stats counters */
							if (temp)
							{
								cmdlist[i].use_count++;
								cmdlist[i].total_bytes+=strlen(temp);
							}

							int MOD_RESULT = 0;
							FOREACH_RESULT(OnPreCommand(command,command_p,items,user));
							if (MOD_RESULT == 1) {
								return;
							}

							/* WARNING: nothing may come after the
							 * command handler call, as the handler
							 * may free the user structure! */

							cmdlist[i].handler_function(command_p,items,user);
						}
						return;
					}
					else
					{
						WriteServ(user->fd,"451 %s :You have not registered",command);
						return;
					}
				}
				cmd_found = 1;
			}
		}
	}
	if ((!cmd_found) && (user))
	{
		statsUnknown++;
		WriteServ(user->fd,"421 %s %s :Unknown command",user->nick,command);
	}
}


void createcommand(char* cmd, handlerfunc f, char flags, int minparams,char* source)
{
	command_t comm;
	/* create the command and push it onto the table */	
	strlcpy(comm.command,cmd,MAXBUF);
	strlcpy(comm.source,source,MAXBUF);
	comm.handler_function = f;
	comm.flags_needed = flags;
	comm.min_params = minparams;
	comm.use_count = 0;
	comm.total_bytes = 0;
	cmdlist.push_back(comm);
	log(DEBUG,"Added command %s (%lu parameters)",cmd,(unsigned long)minparams);
}

bool removecommands(const char* source)
{
	bool go_again = true;
	while (go_again)
	{
		go_again = false;
		for (std::deque<command_t>::iterator i = cmdlist.begin(); i != cmdlist.end(); i++)
		{
			if (!strcmp(i->source,source))
			{
				log(DEBUG,"removecommands(%s) Removing dependent command: %s",i->source,i->command);
				cmdlist.erase(i);
				go_again = true;
				break;
			}
		}
	}
	return true;
}

void SetupCommandTable(void)
{
	createcommand("USER",handle_user,0,4,"<core>");
	createcommand("NICK",handle_nick,0,1,"<core>");
	createcommand("QUIT",handle_quit,0,0,"<core>");
	createcommand("VERSION",handle_version,0,0,"<core>");
	createcommand("PING",handle_ping,0,1,"<core>");
	createcommand("PONG",handle_pong,0,1,"<core>");
	createcommand("ADMIN",handle_admin,0,0,"<core>");
	createcommand("PRIVMSG",handle_privmsg,0,2,"<core>");
	createcommand("INFO",handle_info,0,0,"<core>");
	createcommand("TIME",handle_time,0,0,"<core>");
	createcommand("WHOIS",handle_whois,0,1,"<core>");
	createcommand("WALLOPS",handle_wallops,'o',1,"<core>");
	createcommand("NOTICE",handle_notice,0,2,"<core>");
	createcommand("JOIN",handle_join,0,1,"<core>");
	createcommand("NAMES",handle_names,0,0,"<core>");
	createcommand("PART",handle_part,0,1,"<core>");
	createcommand("KICK",handle_kick,0,2,"<core>");
	createcommand("MODE",handle_mode,0,1,"<core>");
	createcommand("TOPIC",handle_topic,0,1,"<core>");
	createcommand("WHO",handle_who,0,1,"<core>");
	createcommand("MOTD",handle_motd,0,0,"<core>");
	createcommand("RULES",handle_rules,0,0,"<core>");
	createcommand("OPER",handle_oper,0,2,"<core>");
	createcommand("LIST",handle_list,0,0,"<core>");
	createcommand("DIE",handle_die,'o',1,"<core>");
	createcommand("RESTART",handle_restart,'o',1,"<core>");
	createcommand("KILL",handle_kill,'o',2,"<core>");
	createcommand("REHASH",handle_rehash,'o',0,"<core>");
	createcommand("LUSERS",handle_lusers,0,0,"<core>");
	createcommand("STATS",handle_stats,0,1,"<core>");
	createcommand("USERHOST",handle_userhost,0,1,"<core>");
	createcommand("AWAY",handle_away,0,0,"<core>");
	createcommand("ISON",handle_ison,0,0,"<core>");
	createcommand("SUMMON",handle_summon,0,0,"<core>");
	createcommand("USERS",handle_users,0,0,"<core>");
	createcommand("INVITE",handle_invite,0,2,"<core>");
	createcommand("PASS",handle_pass,0,1,"<core>");
	createcommand("TRACE",handle_trace,'o',0,"<core>");
	createcommand("WHOWAS",handle_whowas,0,1,"<core>");
	createcommand("CONNECT",handle_connect,'o',1,"<core>");
	createcommand("SQUIT",handle_squit,'o',0,"<core>");
	createcommand("MODULES",handle_modules,0,0,"<core>");
	createcommand("LINKS",handle_links,0,0,"<core>");
	createcommand("MAP",handle_map,0,0,"<core>");
	createcommand("KLINE",handle_kline,'o',1,"<core>");
	createcommand("GLINE",handle_gline,'o',1,"<core>");
	createcommand("ZLINE",handle_zline,'o',1,"<core>");
	createcommand("QLINE",handle_qline,'o',1,"<core>");
	createcommand("ELINE",handle_eline,'o',1,"<core>");
	createcommand("LOADMODULE",handle_loadmodule,'o',1,"<core>");
	createcommand("UNLOADMODULE",handle_unloadmodule,'o',1,"<core>");
	createcommand("SERVER",handle_server,0,0,"<core>");
}

void process_buffer(const char* cmdbuf,userrec *user)
{
	if (!user)
	{
		log(DEFAULT,"*** BUG *** process_buffer was given an invalid parameter");
		return;
	}
	char cmd[MAXBUF];
	if (!cmdbuf)
	{
		log(DEFAULT,"*** BUG *** process_buffer was given an invalid parameter");
		return;
	}
	if (!cmdbuf[0])
	{
		return;
	}
	while (*cmdbuf == ' ') cmdbuf++; // strip leading spaces

	strlcpy(cmd,cmdbuf,MAXBUF);
	if (!cmd[0])
	{
		return;
	}
	int sl = strlen(cmd)-1;
	if ((cmd[sl] == 13) || (cmd[sl] == 10))
	{
		cmd[sl] = '\0';
	}
	sl = strlen(cmd)-1;
	if ((cmd[sl] == 13) || (cmd[sl] == 10))
	{
		cmd[sl] = '\0';
	}
	sl = strlen(cmd)-1;
	while (cmd[sl] == ' ') // strip trailing spaces
	{
		cmd[sl] = '\0';
		sl = strlen(cmd)-1;
	}

	if (!cmd[0])
	{
		return;
	}
        log(DEBUG,"CMDIN: %s %s",user->nick,cmd);
	tidystring(cmd);
	if ((user) && (cmd))
	{
		process_command(user,cmd);
	}
}

void DoSync(serverrec* serv, char* tcp_host)
{
	char data[MAXBUF];
	log(DEBUG,"Sending sync");
	// send start of sync marker: Y <timestamp>
	// at this point the ircd receiving it starts broadcasting this netburst to all ircds
	// except the ones its receiving it from.
	snprintf(data,MAXBUF,"Y %lu",(unsigned long)TIME);
	serv->SendPacket(data,tcp_host);
	// send users and channels

	NetSendMyRoutingTable();

	// send all routing table and uline voodoo. The ordering of these commands is IMPORTANT!
        for (int j = 0; j < 32; j++)
        {
                if (me[j] != NULL)
                {
                        for (int k = 0; k < me[j]->connectors.size(); k++)
                        {
                                if (is_uline(me[j]->connectors[k].GetServerName().c_str()))
                                {
                                        snprintf(data,MAXBUF,"H %s",me[j]->connectors[k].GetServerName().c_str());
                                        serv->SendPacket(data,tcp_host);
                                }
                        }
                }
        }

	// send our version for the remote side to cache
	snprintf(data,MAXBUF,"v %s %s",ServerName,GetVersionString().c_str());
	serv->SendPacket(data,tcp_host);

	// sync the users and channels, give the modules a look-in.
	for (user_hash::iterator u = clientlist.begin(); u != clientlist.end(); u++)
	{
		snprintf(data,MAXBUF,"N %lu %s %s %s %s +%s %s %s :%s",(unsigned long)u->second->age,u->second->nick,u->second->host,u->second->dhost,u->second->ident,u->second->modes,u->second->ip,u->second->server,u->second->fullname);
		serv->SendPacket(data,tcp_host);
		if (strchr(u->second->modes,'o'))
		{
			snprintf(data,MAXBUF,"| %s %s",u->second->nick,u->second->oper);
			serv->SendPacket(data,tcp_host);
		}
		for (int i = 0; i <= MODCOUNT; i++)
		{
			string_list l = modules[i]->OnUserSync(u->second);
			for (int j = 0; j < l.size(); j++)
			{
				strlcpy(data,l[j].c_str(),MAXBUF);
  				serv->SendPacket(data,tcp_host);
  			}
  		}
		char* chl = chlist(u->second,u->second);
		if (strcmp(chl,""))
		{
			snprintf(data,MAXBUF,"J %s %s",u->second->nick,chl);
			serv->SendPacket(data,tcp_host);
		}
	}
	// send channel modes, topics etc...
	for (chan_hash::iterator c = chanlist.begin(); c != chanlist.end(); c++)
	{
		snprintf(data,MAXBUF,"M %s +%s",c->second->name,chanmodes(c->second));
		serv->SendPacket(data,tcp_host);
		for (int i = 0; i <= MODCOUNT; i++)
		{
			string_list l = modules[i]->OnChannelSync(c->second);
			for (int j = 0; j < l.size(); j++)
			{
				strlcpy(data,l[j].c_str(),MAXBUF);
  				serv->SendPacket(data,tcp_host);
  			}
  		}
		if (c->second->topic[0])
		{
			snprintf(data,MAXBUF,"T %lu %s %s :%s",(unsigned long)c->second->topicset,c->second->setby,c->second->name,c->second->topic);
			serv->SendPacket(data,tcp_host);
		}
		// send current banlist
		
		for (BanList::iterator b = c->second->bans.begin(); b != c->second->bans.end(); b++)
		{
			snprintf(data,MAXBUF,"M %s +b %s",c->second->name,b->data);
			serv->SendPacket(data,tcp_host);
		}
	}
	// sync global zlines, glines, etc
	sync_xlines(serv,tcp_host);

	snprintf(data,MAXBUF,"F %lu",(unsigned long)TIME);
	serv->SendPacket(data,tcp_host);
	log(DEBUG,"Sent sync");
	// ircd sends its serverlist after the end of sync here
}


void NetSendMyRoutingTable()
{
	// send out a line saying what is reachable to us.
	// E.g. if A is linked to B C and D, send out:
	// $ A B C D
	// if its only linked to B and D send out:
	// $ A B D
	// if it has no links, dont even send out the line at all.
	char buffer[MAXBUF];
	snprintf(buffer,MAXBUF,"$ %s",ServerName);
	bool sendit = false;
	for (int i = 0; i < 32; i++)
	{
		if (me[i] != NULL)
		{
			for (int j = 0; j < me[i]->connectors.size(); j++)
			{
				if ((me[i]->connectors[j].GetState() != STATE_DISCONNECTED) || (is_uline(me[i]->connectors[j].GetServerName().c_str())))
				{
					strlcat(buffer," ",MAXBUF);
					strlcat(buffer,me[i]->connectors[j].GetServerName().c_str(),MAXBUF);
					sendit = true;
				}
			}
		}
	}
	if (sendit)
		NetSendToAll(buffer);
}


void DoSplit(const char* params)
{
	bool go_again = true;
	while (go_again)
	{
		go_again = false;
		for (int i = 0; i < 32; i++)
		{
			if (me[i] != NULL)
			{
				for (vector<ircd_connector>::iterator j = me[i]->connectors.begin(); j != me[i]->connectors.end(); j++)
				{
					if (!strcasecmp(j->GetServerName().c_str(),params))
					{
						j->routes.clear();
						j->CloseConnection();
						me[i]->connectors.erase(j);
						go_again = true;
						break;
					}
				}
			}
		}
	}
	log(DEBUG,"Removed server. Will remove clients...");
	// iterate through the userlist and remove all users on this server.
	// because we're dealing with a mesh, we dont have to deal with anything
	// "down-route" from this server (nice huh)
	go_again = true;
	char reason[MAXBUF];
	snprintf(reason,MAXBUF,"%s %s",ServerName,params);
	while (go_again)
	{
		go_again = false;
		for (user_hash::const_iterator u = clientlist.begin(); u != clientlist.end(); u++)
		{
			if (!strcasecmp(u->second->server,params))
			{
				kill_link(u->second,reason);
				go_again = true;
				break;
			}
		}
	}
}

// removes a server. Will NOT remove its users!

void RemoveServer(const char* name)
{
	bool go_again = true;
	while (go_again)
	{
		go_again = false;
		for (int i = 0; i < 32; i++)
		{
			if (me[i] != NULL)
			{
				for (vector<ircd_connector>::iterator j = me[i]->connectors.begin(); j != me[i]->connectors.end(); j++)
				{
					if (!strcasecmp(j->GetServerName().c_str(),name))
					{
						j->routes.clear();
						j->CloseConnection();
						me[i]->connectors.erase(j);
						go_again = true;
						break;
					}
				}
			}
		}
	}
}


char MODERR[MAXBUF];

char* ModuleError()
{
	return MODERR;
}

void erase_factory(int j)
{
	int v = 0;
	for (std::vector<ircd_module*>::iterator t = factory.begin(); t != factory.end(); t++)
	{
		if (v == j)
		{
                	factory.erase(t);
                 	factory.push_back(NULL);
                 	return;
           	}
		v++;
     	}
}

void erase_module(int j)
{
	int v1 = 0;
	for (std::vector<Module*>::iterator m = modules.begin(); m!= modules.end(); m++)
        {
                if (v1 == j)
                {
			delete *m;
                        modules.erase(m);
                        modules.push_back(NULL);
			break;
                }
		v1++;
        }
	int v2 = 0;
        for (std::vector<std::string>::iterator v = module_names.begin(); v != module_names.end(); v++)
        {
                if (v2 == j)
                {
                       module_names.erase(v);
                       break;
                }
		v2++;
        }

}

bool UnloadModule(const char* filename)
{
	std::string filename_str = filename;
	for (int j = 0; j != module_names.size(); j++)
	{
		if (module_names[j] == filename_str)
		{
			if (modules[j]->GetVersion().Flags & VF_STATIC)
			{
				log(DEFAULT,"Failed to unload STATIC module %s",filename);
				snprintf(MODERR,MAXBUF,"Module not unloadable (marked static)");
				return false;
			}
			// found the module
			log(DEBUG,"Deleting module...");
			erase_module(j);
			log(DEBUG,"Erasing module entry...");
			erase_factory(j);
                        log(DEBUG,"Removing dependent commands...");
                        removecommands(filename);
			log(DEFAULT,"Module %s unloaded",filename);
			MODCOUNT--;
			return true;
		}
	}
	log(DEFAULT,"Module %s is not loaded, cannot unload it!",filename);
	snprintf(MODERR,MAXBUF,"Module not loaded");
	return false;
}

bool DirValid(char* dirandfile)
{
	char work[MAXBUF];
	strlcpy(work,dirandfile,MAXBUF);
	int p = strlen(work);
	// we just want the dir
	while (strlen(work))
	{
		if (work[p] == '/')
		{
			work[p] = '\0';
			break;
		}
		work[p--] = '\0';
	}
	char buffer[MAXBUF], otherdir[MAXBUF];
	// Get the current working directory
	if( getcwd( buffer, MAXBUF ) == NULL )
		return false;
	chdir(work);
	if( getcwd( otherdir, MAXBUF ) == NULL )
		return false;
	chdir(buffer);
	if (strlen(otherdir) >= strlen(work))
	{
		otherdir[strlen(work)] = '\0';
		if (!strcmp(otherdir,work))
		{
			return true;
		}
		return false;
	}
	else return false;
}

std::string GetFullProgDir(char** argv, int argc)
{
	char work[MAXBUF];
	strlcpy(work,argv[0],MAXBUF);
	int p = strlen(work);
	// we just want the dir
	while (strlen(work))
	{
		if (work[p] == '/')
		{
			work[p] = '\0';
			break;
		}
		work[p--] = '\0';
	}
	char buffer[MAXBUF], otherdir[MAXBUF];
	// Get the current working directory
	if( getcwd( buffer, MAXBUF ) == NULL )
		return "";
	chdir(work);
	if( getcwd( otherdir, MAXBUF ) == NULL )
		return "";
	chdir(buffer);
	return otherdir;
}

bool LoadModule(const char* filename)
{
	char modfile[MAXBUF];
	snprintf(modfile,MAXBUF,"%s/%s",ModPath,filename);
	std::string filename_str = filename;
	if (!DirValid(modfile))
	{
		log(DEFAULT,"Module %s is not within the modules directory.",modfile);
		snprintf(MODERR,MAXBUF,"Module %s is not within the modules directory.",modfile);
		return false;
	}
	log(DEBUG,"Loading module: %s",modfile);
        if (FileExists(modfile))
        {
		for (int j = 0; j < module_names.size(); j++)
		{
			if (module_names[j] == filename_str)
			{
				log(DEFAULT,"Module %s is already loaded, cannot load a module twice!",modfile);
				snprintf(MODERR,MAXBUF,"Module already loaded");
				return false;
			}
		}
		ircd_module* a = new ircd_module(modfile);
                factory[MODCOUNT+1] = a;
                if (factory[MODCOUNT+1]->LastError())
                {
                        log(DEFAULT,"Unable to load %s: %s",modfile,factory[MODCOUNT+1]->LastError());
			snprintf(MODERR,MAXBUF,"Loader/Linker error: %s",factory[MODCOUNT+1]->LastError());
			MODCOUNT--;
			return false;
                }
                if (factory[MODCOUNT+1]->factory)
                {
			Module* m = factory[MODCOUNT+1]->factory->CreateModule();
                        modules[MODCOUNT+1] = m;
                        /* save the module and the module's classfactory, if
                         * this isnt done, random crashes can occur :/ */
                        module_names.push_back(filename);
                }
		else
                {
                        log(DEFAULT,"Unable to load %s",modfile);
			snprintf(MODERR,MAXBUF,"Factory function failed!");
			return false;
                }
        }
        else
        {
                log(DEFAULT,"InspIRCd: startup: Module Not Found %s",modfile);
		snprintf(MODERR,MAXBUF,"Module file could not be found");
		return false;
        }
	MODCOUNT++;
	return true;
}

int InspIRCd(char** argv, int argc)
{
	struct sockaddr_in client,server;
	char addrs[MAXBUF][255];
	int incomingSockfd, result = TRUE;
	socklen_t length;
	int count = 0;
	int selectResult = 0, selectResult2 = 0;
	char configToken[MAXBUF], Addr[MAXBUF], Type[MAXBUF];
	fd_set selectFds;
	timeval tv;

	std::string logpath = GetFullProgDir(argv,argc) + "/ircd.log";
	log_file = fopen(logpath.c_str(),"a+");
	if (!log_file)
	{
		printf("ERROR: Could not write to logfile %s, bailing!\n\n",logpath.c_str());
		Exit(ERROR);
	}
	printf("Logging to %s...\n",logpath.c_str());

	log(DEFAULT,"$Id$");
	if (geteuid() == 0)
	{
		printf("WARNING!!! You are running an irc server as ROOT!!! DO NOT DO THIS!!!\n\n");
		Exit(ERROR);
		log(DEFAULT,"InspIRCd: startup: not starting with UID 0!");
	}
	SetupCommandTable();
	log(DEBUG,"InspIRCd: startup: default command table set up");
	
	ReadConfig(true,NULL);
	if (DieValue[0])
	{ 
		printf("WARNING: %s\n\n",DieValue);
		log(DEFAULT,"Ut-Oh, somebody didn't read their config file: '%s'",DieValue);
		exit(0); 
	}  
	log(DEBUG,"InspIRCd: startup: read config");

	int clientportcount = 0, serverportcount = 0;

	for (count = 0; count < ConfValueEnum("bind",&config_f); count++)
	{
		ConfValue("bind","port",count,configToken,&config_f);
		ConfValue("bind","address",count,Addr,&config_f);
		ConfValue("bind","type",count,Type,&config_f);
		if (!strcmp(Type,"servers"))
		{
			char Default[MAXBUF];
			strcpy(Default,"no");
			ConfValue("bind","default",count,Default,&config_f);
			if (strchr(Default,'y'))
			{
				defaultRoute = serverportcount;
				log(DEBUG,"InspIRCd: startup: binding '%s:%s' is default server route",Addr,configToken);
			}
			me[serverportcount] = new serverrec(ServerName,100L,false);
			if (!me[serverportcount]->CreateListener(Addr,atoi(configToken)))
			{
				log(DEFAULT,"Warning: Failed to bind port %lu",(unsigned long)atoi(configToken));
				printf("Warning: Failed to bind port %lu\n",(unsigned long)atoi(configToken));
			}
			else
			{
				serverportcount++;
			}
		}
		else
		{
			ports[clientportcount] = atoi(configToken);
			strlcpy(addrs[clientportcount],Addr,256);
			clientportcount++;
		}
		log(DEBUG,"InspIRCd: startup: read binding %s:%s [%s] from config",Addr,configToken, Type);
	}
	portCount = clientportcount;
	SERVERportCount = serverportcount;
	  
	log(DEBUG,"InspIRCd: startup: read %lu total client ports and %lu total server ports",(unsigned long)portCount,(unsigned long)SERVERportCount);
	log(DEBUG,"InspIRCd: startup: InspIRCd is now starting!");
	
	printf("\n");
	
	/* BugFix By Craig! :p */
	MODCOUNT = -1;
	for (count = 0; count < ConfValueEnum("module",&config_f); count++)
	{
		ConfValue("module","name",count,configToken,&config_f);
		printf("Loading module... \033[1;32m%s\033[0m\n",configToken);
		if (!LoadModule(configToken))
		{
			log(DEFAULT,"Exiting due to a module loader error.");
			printf("\nThere was an error loading a module: %s\n\nYou might want to do './inspircd start' instead of 'bin/inspircd'\n\n",ModuleError());
			Exit(0);
		}
	}
	log(DEFAULT,"Total loaded modules: %lu",(unsigned long)MODCOUNT+1);
	
	startup_time = time(NULL);
	  
	char PID[MAXBUF];
	ConfValue("pid","file",0,PID,&config_f);
	// write once here, to try it out and make sure its ok
	WritePID(PID);
	  
	/* setup select call */
#ifndef USE_KQUEUE
	FD_ZERO(&selectFds);
#endif
	log(DEBUG,"InspIRCd: startup: zero selects");
	log(VERBOSE,"InspIRCd: startup: portCount = %lu", (unsigned long)portCount);
	
	for (count = 0; count < portCount; count++)
	{
		if ((openSockfd[boundPortCount] = OpenTCPSocket()) == ERROR)
		{
			log(DEBUG,"InspIRCd: startup: bad fd %lu",(unsigned long)openSockfd[boundPortCount]);
			return(ERROR);
		}
		if (BindSocket(openSockfd[boundPortCount],client,server,ports[count],addrs[count]) == ERROR)
		{
			log(DEFAULT,"InspIRCd: startup: failed to bind port %lu",(unsigned long)ports[count]);
		}
		else	/* well we at least bound to one socket so we'll continue */
		{
			boundPortCount++;
		}
	}
	
	log(DEBUG,"InspIRCd: startup: total bound ports %lu",(unsigned long)boundPortCount);
	  
	/* if we didn't bind to anything then abort */
	if (boundPortCount == 0)
	{
		log(DEFAULT,"InspIRCd: startup: no ports bound, bailing!");
		printf("\nERROR: Was not able to bind any of %lu ports! Please check your configuration.\n\n", (unsigned long)portCount);
		return (ERROR);
	}
	

        printf("\nInspIRCd is now running!\n");

        if (nofork)
        {
                log(VERBOSE,"Not forking as -nofork was specified");
        }
        else
        {
                if (DaemonSeed() == ERROR)
                {
                        log(DEFAULT,"InspIRCd: startup: can't daemonise");
                        printf("ERROR: could not go into daemon mode. Shutting down.\n");
                        Exit(ERROR);
                }
        }

	// BUGFIX: We cannot initialize this before forking, as the kqueue data is not inherited by child processes!
#ifdef USE_KQUEUE
        kq = kqueue();
	lkq = kqueue();
	skq = kqueue();
        if ((kq == -1) || (lkq == -1) || (skq == -1))
        {
                log(DEFAULT,"main: kqueue() failed!");
                printf("ERROR: could not initialise kqueue event system. Shutting down.\n");
                Exit(ERROR);
        }
#endif


#ifdef USE_KQUEUE
	log(DEFAULT,"kqueue socket engine is enabled. Filling listen list.");
	for (count = 0; count < boundPortCount; count++)
	{
	        struct kevent ke;
	        log(DEBUG,"kqueue: Add listening socket to events, kq=%d socket=%d",lkq,openSockfd[count]);
	        EV_SET(&ke, openSockfd[count], EVFILT_READ, EV_ADD, 0, 5, NULL);
	        int i = kevent(lkq, &ke, 1, 0, 0, NULL);
	        if (i == -1)
	        {
			log(DEFAULT,"main: add listen ports to kqueue failed!");
			printf("ERROR: could not initialise listening sockets in kqueue. Shutting down.\n");
	        }
	}
        for (int t = 0; t != SERVERportCount; t++)
        {
                struct kevent ke;
                if (me[t])
                {
			log(DEBUG,"kqueue: Add listening SERVER socket to events, kq=%d socket=%d",skq,me[t]->fd);
	                EV_SET(&ke, me[t]->fd, EVFILT_READ, EV_ADD, 0, 5, NULL);
	                int i = kevent(skq, &ke, 1, 0, 0, NULL);
	                if (i == -1)
	                {
	                        log(DEFAULT,"main: add server listen ports to kqueue failed!");
	                        printf("ERROR: could not initialise listening server sockets in kqueue. Shutting down.\n");
	                }
		}
        }


#else
	log(DEFAULT,"Using standard select socket engine.");
#endif

	WritePID(PID);

	length = sizeof (client);
	char tcp_msg[MAXBUF],tcp_host[MAXBUF];

#ifdef USE_KQUEUE
        struct kevent ke;
	struct kevent ke_list[33];
        struct timespec ts;
#endif
        fd_set serverfds;
        timeval tvs;
        tvs.tv_usec = 10000L;
        tvs.tv_sec = 0;
	tv.tv_sec = 0;
	tv.tv_usec = 10000L;
        char data[65536];
	timeval tval;
	fd_set sfd;
        tval.tv_usec = 10000L;
        tval.tv_sec = 0;
        int total_in_this_set = 0;
	int i = 0, v = 0, j = 0, cycle_iter = 0;
	bool expire_run = false;
	  
	/* main loop, this never returns */
	for (;;)
	{
#ifdef _POSIX_PRIORITY_SCHEDULING
		sched_yield();
#endif
#ifndef USE_KQUEUE
		FD_ZERO(&sfd);
#endif

		// we only read time() once per iteration rather than tons of times!
		OLDTIME = TIME;
		TIME = time(NULL);

		dns_poll();

		// *FIX* Instead of closing sockets in kill_link when they receive the ERROR :blah line, we should queue
		// them in a list, then reap the list every second or so.
		if (((TIME % 5) == 0) && (!expire_run))
		{
			expire_lines();
			FOREACH_MOD OnBackgroundTimer(TIME);
			expire_run = true;
			continue;
		}
		if ((TIME % 5) == 1)
			expire_run = false;
		
		// fix by brain - this must be below any manipulation of the hashmap by modules
		user_hash::iterator count2 = clientlist.begin();

#ifdef USE_KQUEUE
		ts.tv_sec = 0;
		ts.tv_nsec = 30000L;
		i = kevent(skq, NULL, 0, &ke, 1, &ts);
		if (i > 0)
		{
		        log(DEBUG,"kqueue: Listening server socket event, i=%d, ke.ident=%d",i,ke.ident);
		        for (int x = 0; x != SERVERportCount; x++)
		        {
		                if ((me[x]) && (ke.ident == me[x]->fd))
		                {

#else
		FD_ZERO(&serverfds);
		for (int x = 0; x != SERVERportCount; x++)
		{
			if (me[x])
				FD_SET(me[x]->fd, &serverfds);
		}
		tvs.tv_usec = 30000L;
		tvs.tv_sec = 0;
		int servresult = select(32767, &serverfds, NULL, NULL, &tvs);
		if (servresult > 0)
		{
			for (int x = 0; x != SERVERportCount; x++)
			{
				if ((me[x]) && (FD_ISSET (me[x]->fd, &serverfds)))
				{
#endif
					char remotehost[MAXBUF],resolved[MAXBUF];
					length = sizeof (client);
					incomingSockfd = accept (me[x]->fd, (sockaddr *) &client, &length);
					if (incomingSockfd != -1)
					{
						strlcpy(remotehost,(char *)inet_ntoa(client.sin_addr),MAXBUF);
						if(CleanAndResolve(resolved, remotehost) != TRUE)
						{
							strlcpy(resolved,remotehost,MAXBUF);
						}
						// add to this connections ircd_connector vector
						// *FIX* - we need the LOCAL port not the remote port in &client!
						me[x]->AddIncoming(incomingSockfd,resolved,me[x]->port);
					}
				}
			}
		}
     
		for (int x = 0; x < SERVERportCount; x++)
		{
			std::deque<std::string> msgs;
			msgs.clear();
			if ((me[x]) && (me[x]->RecvPacket(msgs, tcp_host)))
			{
				for (int ctr = 0; ctr < msgs.size(); ctr++)
				{
					strlcpy(tcp_msg,msgs[ctr].c_str(),MAXBUF);
					log(DEBUG,"Processing: %s",tcp_msg);
					if (!tcp_msg[0])
    					{
						log(DEBUG,"Invalid string from %s [route%lu]",tcp_host,(unsigned long)x);
						break;
					}
					// during a netburst, send all data to all other linked servers
					if ((((nb_start>0) && (tcp_msg[0] != 'Y') && (tcp_msg[0] != 'X') && (tcp_msg[0] != 'F'))) || (is_uline(tcp_host)))
					{
						if (is_uline(tcp_host))
						{
							if ((tcp_msg[0] != 'Y') && (tcp_msg[0] != 'X') && (tcp_msg[0] != 'F'))
							{
								NetSendToAllExcept(tcp_host,tcp_msg);
							}
						}
						else
							NetSendToAllExcept(tcp_host,tcp_msg);
					}
		                        std::string msg = tcp_msg;
		                        FOREACH_MOD OnPacketReceive(msg,tcp_host);
		                        strlcpy(tcp_msg,msg.c_str(),MAXBUF);
					handle_link_packet(tcp_msg, tcp_host, me[x]);
				}
				goto label;
			}
		}
	
	while (count2 != clientlist.end())
	{
#ifndef USE_KQUEUE
		FD_ZERO(&sfd);
#endif

		total_in_this_set = 0;

		user_hash::iterator xcount = count2;
		user_hash::iterator endingiter = count2;

		if (count2 == clientlist.end()) break;

		userrec* curr = NULL;

		if (count2->second)
			curr = count2->second;

		if ((curr) && (curr->fd != 0))
		{
#ifdef _POSIX_PRIORITY_SCHEDULING
        sched_yield();
#endif
			// assemble up to 64 sockets into an fd_set
			// to implement a pooling mechanism.
			//
			// This should be up to 64x faster than the
			// old implementation.
#ifndef USE_KQUEUE
			while (total_in_this_set < 1024)
			{
				if (count2 != clientlist.end())
				{
					curr = count2->second;
					// we don't check the state of remote users.
					if ((curr->fd != -1) && (curr->fd != FD_MAGIC_NUMBER))
					{
						FD_SET (curr->fd, &sfd);

						// registration timeout -- didnt send USER/NICK/HOST in the time specified in
						// their connection class.
						if ((TIME > curr->timeout) && (curr->registered != 7)) 
						{
						  	log(DEBUG,"InspIRCd: registration timeout: %s",curr->nick);
							kill_link(curr,"Registration timeout");
							goto label;
						}
						if ((TIME > curr->signon) && (curr->registered == 3) && (AllModulesReportReady(curr)))
						{
							log(DEBUG,"signon exceed, registered=3, and modules ready, OK");
							curr->dns_done = true;
							statsDnsBad++;
							FullConnectUser(curr);
							goto label;
						}
		                                if ((curr->dns_done) && (curr->registered == 3) && (AllModulesReportReady(curr))) // both NICK and USER... and DNS
		                                {
							log(DEBUG,"dns done, registered=3, and modules ready, OK");
		                                        FullConnectUser(curr);
							goto label;
		                                }
						if ((TIME > curr->nping) && (isnick(curr->nick)) && (curr->registered == 7))
						{
							if ((!curr->lastping) && (curr->registered == 7))
							{
							  	log(DEBUG,"InspIRCd: ping timeout: %s",curr->nick);
								kill_link(curr,"Ping timeout");
								goto label;
							}
							Write(curr->fd,"PING :%s",ServerName);
						  	log(DEBUG,"InspIRCd: pinging: %s",curr->nick);
							curr->lastping = 0;
							curr->nping = TIME+curr->pingmax;	// was hard coded to 120
						}
					}
					count2++;
					total_in_this_set++;
				}
				else break;
			}
	       		endingiter = count2;
       			count2 = xcount; // roll back to where we were
#else
			// KQUEUE: We don't go through a loop to fill the fd_set so instead we must manually do this loop every now and again.
			// TODO: We dont need to do all this EVERY loop iteration, tone down the visits to this if we're using kqueue.
			cycle_iter++;
			if (cycle_iter > 10) while (count2 != clientlist.end())
			{
				cycle_iter = 0;
				if (count2 != clientlist.end())
				{
                	                curr = count2->second;
        	                        // we don't check the state of remote users.
	                                if ((curr->fd != -1) && (curr->fd != FD_MAGIC_NUMBER))
					{
	                                        // registration timeout -- didnt send USER/NICK/HOST in the time specified in
	                                        // their connection class.
	                                        if ((TIME > curr->timeout) && (curr->registered != 7))
	                                        {
	                                                log(DEBUG,"InspIRCd: registration timeout: %s",curr->nick);
	                                                kill_link(curr,"Registration timeout");
        	                                        goto label;
	       	                                }
	       	                                if ((TIME > curr->signon) && (curr->registered == 3) && (AllModulesReportReady(curr)))
	       	                                {
	                                                log(DEBUG,"signon exceed, registered=3, and modules ready, OK: %d %d",TIME,curr->signon);
        	                                        curr->dns_done = true;
	                                                statsDnsBad++;
	                                                FullConnectUser(curr);
	                                                goto label;
        	                                }
	                                        if ((curr->dns_done) && (curr->registered == 3) && (AllModulesReportReady(curr)))
        	                                {
	                                                log(DEBUG,"dns done, registered=3, and modules ready, OK");
	                                                FullConnectUser(curr);
	                                                goto label;
	                                        }
						if ((TIME > curr->nping) && (isnick(curr->nick)) && (curr->registered == 7))
						{
	                                         	if ((!curr->lastping) && (curr->registered == 7))
	                                        	{
	                                        	        log(DEBUG,"InspIRCd: ping timeout: %s",curr->nick);
	                                        	        kill_link(curr,"Ping timeout");
	                                        	        goto label;
		                                       	}
		                                       	Write(curr->fd,"PING :%s",ServerName);
	                                        	log(DEBUG,"InspIRCd: pinging: %s",curr->nick);
	                                        	curr->lastping = 0;
	                                                curr->nping = TIME+curr->pingmax;       // was hard coded to 120
						}
					}
				}
				else break;
				count2++;
			}
			// increment the counter right to the end of the list, as kqueue processes everything in one go
#endif
        
        		v = 0;

#ifdef USE_KQUEUE
			ts.tv_sec = 0;
			ts.tv_nsec = 1000L;
			// for now, we only read 1 event. We could read soooo many more :)
			int i = kevent(kq, NULL, 0, &ke, 1, &ts);
			if (i > 0)
			{
				log(DEBUG,"kevent call: kq=%d, i=%d",kq,i);
				// KQUEUE: kevent gives us ONE fd which is ready to have something done to it. Do something to it.
				userrec* cu = fd_ref_table[ke.ident];
#else
			tval.tv_usec = 1000L;
			selectResult2 = select(65535, &sfd, NULL, NULL, &tval);
			
			// now loop through all of the items in this pool if any are waiting
			if (selectResult2 > 0)
			for (user_hash::iterator count2a = xcount; count2a != endingiter; count2a++)
			{
				// SELECT: we have to iterate...
				userrec* cu = count2a->second;
#endif

#ifdef _POSIX_PRIORITY_SCHEDULING
				sched_yield();
#endif
				result = EAGAIN;
#ifdef USE_KQUEUE
				// KQUEUE: We already know we have a valid FD. No checks needed.
				if ((cu->fd != FD_MAGIC_NUMBER) && (cu->fd != -1))
#else
				// SELECT: We don't know if our FD is valid.
				if ((cu->fd != FD_MAGIC_NUMBER) && (cu->fd != -1) && (FD_ISSET (cu->fd, &sfd)))
#endif
				{
					log(DEBUG,"Data waiting on socket %d",cu->fd);
			                int MOD_RESULT = 0;
					int result2 = 0;
			                FOREACH_RESULT(OnRawSocketRead(cu->fd,data,65535,result2));
				        if (!MOD_RESULT)
					{
						result = read(cu->fd, data, 65535);
					}
					else result = result2;
					log(DEBUG,"Read result: %d",result);
					if (result)
					{
						statsRecv += result;
						// perform a check on the raw buffer as an array (not a string!) to remove
						// characters 0 and 7 which are illegal in the RFC - replace them with spaces.
						// hopefully this should stop even more people whining about "Unknown command: *"
						for (int checker = 0; checker < result; checker++)
						{
							if ((data[checker] == 0) || (data[checker] == 7))
								data[checker] = ' ';
						}
						if (result > 0)
							data[result] = '\0';
						userrec* current = cu;
						int currfd = current->fd;
						int floodlines = 0;
						// add the data to the users buffer
						if (result > 0)
						if (!current->AddBuffer(data))
						{
							// AddBuffer returned false, theres too much data in the user's buffer and theyre up to no good.
                                                        if (current->registered == 7)
                                                        {
                                                                kill_link(current,"RecvQ exceeded");
                                                        }
                                                        else
                                                        {
                                                                WriteOpers("*** Excess flood from %s",current->ip);
                                                                log(DEFAULT,"Excess flood from: %s",current->ip);
                                                                add_zline(120,ServerName,"Flood from unregistered connection",current->ip);
                                                                apply_lines();
                                                        }
                                                        goto label;
						}
						if (current->recvq.length() > NetBufferSize)
						{
							if (current->registered == 7)
							{
								kill_link(current,"RecvQ exceeded");
							}
							else
							{
								WriteOpers("*** Excess flood from %s",current->ip);
								log(DEFAULT,"Excess flood from: %s",current->ip);
								add_zline(120,ServerName,"Flood from unregistered connection",current->ip);
								apply_lines();
							}
							goto label;
						}
						// while there are complete lines to process...
						while (current->BufferIsReady())
						{
							floodlines++;
							if (TIME > current->reset_due)
							{
								current->reset_due = TIME + current->threshold;
								current->lines_in = 0;
							}
							current->lines_in++;
							if (current->lines_in > current->flood)
							{
								log(DEFAULT,"Excess flood from: %s!%s@%s",current->nick,current->ident,current->host);
								WriteOpers("*** Excess flood from: %s!%s@%s",current->nick,current->ident,current->host);
								kill_link(current,"Excess flood");
								goto label;
							}
							if ((floodlines > current->flood) && (current->flood != 0))
							{
								if (current->registered == 7)
								{
								  	log(DEFAULT,"Excess flood from: %s!%s@%s",current->nick,current->ident,current->host);
								  	WriteOpers("*** Excess flood from: %s!%s@%s",current->nick,current->ident,current->host);
									kill_link(current,"Excess flood");
								}
								else
								{
	                                                                add_zline(120,ServerName,"Flood from unregistered connection",current->ip);
        	                                                        apply_lines();
								}
								goto label;
							}
							char sanitized[MAXBUF];
							// use GetBuffer to copy single lines into the sanitized string
							std::string single_line = current->GetBuffer();
                                                        current->bytes_in += single_line.length();
                                                        current->cmds_in++;
							if (single_line.length()>512)
							{
								log(DEFAULT,"Excess flood from: %s!%s@%s",current->nick,current->ident,current->host);
								WriteOpers("*** Excess flood from: %s!%s@%s",current->nick,current->ident,current->host);
								kill_link(current,"Excess flood");
								goto label;
							}
							strlcpy(sanitized,single_line.c_str(),MAXBUF);
							if (*sanitized)
							{
								userrec* old_comp = fd_ref_table[currfd];
								// we're gonna re-scan to check if the nick is gone, after every
								// command - if it has, we're gonna bail
								process_buffer(sanitized,current);
								// look for the user's record in case it's changed... if theyve quit,
								// we cant do anything more with their buffer, so bail.
								// there used to be an ugly, slow loop here. Now we have a reference
								// table, life is much easier (and FASTER)
								userrec* new_comp = fd_ref_table[currfd];
								if ((currfd < 0) || (!fd_ref_table[currfd]) || (old_comp != new_comp))
									goto label;

							}
						}
						goto label;
					}

					if ((result == -1) && (errno != EAGAIN) && (errno != EINTR))
					{
						log(DEBUG,"killing: %s",cu->nick);
						kill_link(cu,strerror(errno));
						goto label;
					}
				}
				// result EAGAIN means nothing read
				if (result == EAGAIN)
				{
				}
				else
				if (result == 0)
				{
#ifndef USE_KQUEUE
				  	if (count2->second)
				  	{
#endif
					  	log(DEBUG,"InspIRCd: Exited: %s",cu->nick);
						kill_link(cu,"Client exited");
						// must bail here? kill_link removes the hash, corrupting the iterator
						log(DEBUG,"Bailing from client exit");
						goto label;
#ifndef USE_KQUEUE
					}
#endif
				}
				else if (result > 0)
				{
				}
			}
		}
		for (int q = 0; q < total_in_this_set; q++)
		{
			count2++;
		}
	}

#ifdef _POSIX_PRIORITY_SCHEDULING
        sched_yield();
#endif
	
#ifndef USE_KQUEUE
	// set up select call
	for (count = 0; count < boundPortCount; count++)
	{
		FD_SET (openSockfd[count], &selectFds);
	}

	tv.tv_usec = 30000L;
	selectResult = select(MAXSOCKS, &selectFds, NULL, NULL, &tv);

	/* select is reporting a waiting socket. Poll them all to find out which */
	if (selectResult > 0)
	{
		for (count = 0; count < boundPortCount; count++)
		{
			if (FD_ISSET (openSockfd[count], &selectFds))
			{
#else
	ts.tv_sec = 0;
	ts.tv_nsec = 30000L;
	i = kevent(lkq, NULL, 0, ke_list, 32, &ts);
	if (i > 0) for (j = 0; j < i; j++)
	{
		log(DEBUG,"kqueue: Listening socket event, i=%d, ke.ident=%d",i,ke.ident);
		// this isnt as efficient as it could be, we could create a reference table
		// to reference bound ports by fd, but this isnt a big bottleneck as the actual
		// number of listening ports on the average ircd is a small number (less than 20)
		// compared to the number of clients (possibly over 2000)
		for (count = 0; count < boundPortCount; count++)
		{
			if (ke_list[j].ident == openSockfd[count])
			{
#endif
				char target[MAXBUF], resolved[MAXBUF];
				length = sizeof (client);
				incomingSockfd = accept (openSockfd[count], (struct sockaddr *) &client, &length);
			      
				strlcpy (target, (char *) inet_ntoa (client.sin_addr), MAXBUF);
				strlcpy (resolved, target, MAXBUF);
			
				if (incomingSockfd < 0)
				{
					WriteOpers("*** WARNING: Accept failed on port %lu (%s)",(unsigned long)ports[count],target);
					log(DEBUG,"InspIRCd: accept failed: %lu",(unsigned long)ports[count]);
					statsRefused++;
				}
				else
				{
					FOREACH_MOD OnRawSocketAccept(incomingSockfd, resolved, ports[count]);
					statsAccept++;
					AddClient(incomingSockfd, resolved, ports[count], false, inet_ntoa (client.sin_addr));
					log(DEBUG,"InspIRCd: adding client on port %lu fd=%lu",(unsigned long)ports[count],(unsigned long)incomingSockfd);
				}
				//goto label;
			}
		}
	}
	label:
	if (0) {};
#ifdef _POSIX_PRIORITY_SCHEDULING
        sched_yield();
	sched_yield();
#endif
}
/* not reached */
close (incomingSockfd);
}

