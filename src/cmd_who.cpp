/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2006 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *                <Craig@chatspike.net>
 *
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "configreader.h"
#include "users.h"
#include "message.h"
#include "modules.h"
#include "commands.h"
#include "helperfuncs.h"
#include "commands/cmd_who.h"

extern ServerConfig* Config;
extern user_hash clientlist;
extern chan_hash chanlist;
extern std::vector<userrec*> all_opers;

void cmd_who::Handle (const char** parameters, int pcnt, userrec *user)
{
	chanrec* Ptr = NULL;
	char tmp[10];
	
	if (pcnt == 1)
	{
		if ((IS_SINGLE(parameters[0],'0')) || (IS_SINGLE(parameters[0],'*')))
		{
			if ((user->chans.size()) && (((ucrec*)*(user->chans.begin()))->channel))
			{
				int n_list = 0;
			  	for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
				{
					Ptr = ((ucrec*)*(i->second->chans.begin()))->channel;
					// suggested by phidjit and FCS
					if ((!common_channels(user,i->second)) && (isnick(i->second->nick)))
					{
						// Bug Fix #29
						*tmp = 0;
						if (*i->second->awaymsg) {
							charlcat(tmp, 'G', 9);
						} else {
							charlcat(tmp, 'H', 9);
						}
						if (*i->second->oper) { charlcat(tmp, '*', 9); }
						WriteServ(user->fd,"352 %s %s %s %s %s %s %s :0 %s",user->nick, Ptr ? Ptr->name : "*", i->second->ident, i->second->dhost, i->second->server, i->second->nick, tmp, i->second->fullname);
						if (n_list++ > Config->MaxWhoResults)
						{
							WriteServ(user->fd,"523 %s WHO :Command aborted: More results than configured limit",user->nick);
							break;
						}
					}
				}
			}
			if (Ptr)
			{
				WriteServ(user->fd,"315 %s %s :End of /WHO list.",user->nick , parameters[0]);
			}
			else
			{
				WriteServ(user->fd,"315 %s %s :End of /WHO list.",user->nick, parameters[0]);
			}
			return;
		}
		if (parameters[0][0] == '#')
		{
			Ptr = FindChan(parameters[0]);
			if (Ptr)
			{
				int n_list = 0;
			  	for (user_hash::const_iterator i = clientlist.begin(); i != clientlist.end(); i++)
				{
					if ((Ptr->HasUser(i->second)) && (isnick(i->second->nick)))
					{
						// Fix Bug #29 - Part 2..
						*tmp = 0;
						if (*i->second->awaymsg) {
							charlcat(tmp, 'G', 9);
						} else {
							charlcat(tmp, 'H', 9);
						}
						if (*i->second->oper) { charlcat(tmp, '*', 9); }
						strlcat(tmp, cmode(i->second, Ptr),5);
						WriteServ(user->fd,"352 %s %s %s %s %s %s %s :0 %s",user->nick, Ptr->name, i->second->ident, i->second->dhost, i->second->server, i->second->nick, tmp, i->second->fullname);
						n_list++;
						if (n_list > Config->MaxWhoResults)
						{
							WriteServ(user->fd,"523 %s WHO :Command aborted: More results than configured limit",user->nick);
							break;
						}

					}
				}
				WriteServ(user->fd,"315 %s %s :End of /WHO list.",user->nick, parameters[0]);
			}
			else
			{
				WriteServ(user->fd,"401 %s %s :No such nick/channel",user->nick, parameters[0]);
			}
		}
		else
		{
			userrec* u = Find(parameters[0]);
			if (u)
			{
				// Bug Fix #29 -- Part 29..
				*tmp = 0;
				if (*u->awaymsg) {
					charlcat(tmp, 'G' ,9);
				} else {
					charlcat(tmp, 'H' ,9);
				}
				if (*u->oper) { charlcat(tmp, '*' ,9); }
				WriteServ(user->fd,"352 %s %s %s %s %s %s %s :0 %s",user->nick, u->chans.size() && ((ucrec*)*(u->chans.begin()))->channel ? ((ucrec*)*(u->chans.begin()))->channel->name
				: "*", u->ident, u->dhost, u->server, u->nick, tmp, u->fullname);
			}
			WriteServ(user->fd,"315 %s %s :End of /WHO list.",user->nick, parameters[0]);
		}
	}
	if (pcnt == 2)
	{
		if ((IS_SINGLE(parameters[0],'0')) || (IS_SINGLE(parameters[0],'*')) && (IS_SINGLE(parameters[1],'o')))
		{
		  	for (std::vector<userrec*>::iterator i = all_opers.begin(); i != all_opers.end(); i++)
			{
				// If i were a rich man.. I wouldn't need to me making these bugfixes..
				// But i'm a poor bastard with nothing better to do.
				userrec* oper = *i;
				*tmp = 0;
				if (*oper->awaymsg) {
					charlcat(tmp, 'G' ,9);
				} else {
					charlcat(tmp, 'H' ,9);
				}
				WriteServ(user->fd,"352 %s %s %s %s %s %s %s* :0 %s", user->nick, oper->chans.size() && ((ucrec*)*(oper->chans.begin()))->channel ? ((ucrec*)*(oper->chans.begin()))->channel->name
				: "*", oper->ident, oper->dhost, oper->server, oper->nick, tmp, oper->fullname);
			}
			WriteServ(user->fd,"315 %s %s :End of /WHO list.",user->nick, parameters[0]);
			return;
		}
	}
}
