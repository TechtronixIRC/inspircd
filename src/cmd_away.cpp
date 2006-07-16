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

#include <vector>
#include "inspircd_config.h"
#include "configreader.h"
#include "users.h"
#include "modules.h"
#include "commands.h"
#include "helperfuncs.h"
#include "commands/cmd_away.h"

extern ServerConfig* Config;
extern InspIRCd* ServerInstance;
extern int MODCOUNT;
extern std::vector<Module*> modules;
extern std::vector<ircd_module*> factory;


void cmd_away::Handle (const char** parameters, int pcnt, userrec *user)
{
	if (pcnt)
	{
		strlcpy(user->awaymsg,parameters[0],MAXAWAY);
		WriteServ(user->fd,"306 %s :You have been marked as being away",user->nick);
		FOREACH_MOD(I_OnSetAway,OnSetAway(user));
	}
	else
	{
		*user->awaymsg = 0;
		WriteServ(user->fd,"305 %s :You are no longer marked as being away",user->nick);
		FOREACH_MOD(I_OnCancelAway,OnCancelAway(user));
	}
}
