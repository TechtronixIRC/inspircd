/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2004 ChatSpike-Dev.
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

using namespace std;

#include <stdio.h>
#include <string>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include "users.h"
#include "channels.h"
#include "modules.h"
#include "inspircd.h"
#include "configreader.h"
#include "helperfuncs.h"
#include "m_sql.h"
#include "commands/cmd_oper.h"

/* $ModDesc: Allows storage of oper credentials in an SQL table */

/* Required for the FOREACH_MOD alias (OnOper event) */
extern int MODCOUNT;
extern ServerConfig* Config;
extern std::vector<Module*> modules;
extern std::vector<ircd_module*> factory;

class ModuleSQLOper : public Module
{
	Server* Srv;
	ConfigReader* Conf;
	unsigned long dbid;
	Module* SQLModule;

 public:
	bool ReadConfig()
	{
		dbid = Conf->ReadInteger("sqloper","dbid",0,true);	// database id of a database configured in m_sql (see m_sql config)
		SQLModule = Srv->FindModule("m_sql.so");
		if (!SQLModule)
			Srv->Log(DEFAULT,"WARNING: m_sqloper.so could not initialize because m_sql.so is not loaded. Load the module and rehash your server.");
		return (SQLModule);
	}

	ModuleSQLOper(Server* Me)
		: Module::Module(Me)
	{
		Srv = Me;
		Conf = new ConfigReader();
		ReadConfig();
	}

	virtual void OnRehash(const std::string &parameter)
	{
		DELETE(Conf);
		Conf = new ConfigReader();
		ReadConfig();
	}

	void Implements(char* List)
	{
		List[I_OnRehash] = List[I_OnPreCommand] = 1;
	}

	virtual int OnPreCommand(const std::string &command, const char** parameters, int pcnt, userrec *user, bool validated)
	{
		if ((command == "OPER") && (validated))
		{
			if (LookupOper(parameters[0],parameters[1],user))
				return 1;
		}
		return 0;
	}

	bool LookupOper(const std::string &s_username, const std::string &s_password, userrec* user)
	{
		bool found = false;

		// is the sql module loaded? If not, we don't attempt to do anything.
		if (!SQLModule)
			return false;

		// sanitize the password (we dont want any mysql insertion exploits!)
		std::string username = SQLQuery::Sanitise(s_username);
		std::string password = SQLQuery::Sanitise(s_password);

		// Create a request containing the SQL query and send it to m_sql.so
		SQLRequest* query = new SQLRequest(SQL_RESULT,dbid,"SELECT username,password,hostname,type FROM ircd_opers WHERE username='"+username+"' AND password=md5('"+password+"')");
		Request queryrequest((char*)query, this, SQLModule);
		SQLResult* result = (SQLResult*)queryrequest.Send();

		// Did we get "OK" as a result?
		if (result->GetType() == SQL_OK)
		{
			Srv->Log(DEBUG,"An SQL based oper exists");
			// if we did, this means we may now request a row... there should be only one row for each user, so,
			// we don't need to loop to fetch multiple rows.
			SQLRequest* rowrequest = new SQLRequest(SQL_ROW,dbid,"");
			Request rowquery((char*)rowrequest, this, SQLModule);
			SQLResult* rowresult = (SQLResult*)rowquery.Send();

			// did we get a row? If we did, we can now do something with the fields
			if (rowresult->GetType() == SQL_ROW)
			{
				if (rowresult->GetField("username") == username)
				{
					found = true;
					// oper up the user.
					
					for (int j =0; j < Conf->Enumerate("type"); j++)
					{
						std::string TypeName = Conf->ReadValue("type","name",j);
						Srv->Log(DEBUG,"Scanning opertype: "+TypeName);
						std::string pattern = std::string(user->ident) + "@" + std::string(user->host);
							
						if((TypeName == rowresult->GetField("type")) && OneOfMatches(pattern.c_str(), rowresult->GetField("hostname").c_str()))
						{
							/* found this oper's opertype */
							Srv->Log(DEBUG,"Host and type match: "+TypeName+" "+rowresult->GetField("type"));
							std::string HostName = Conf->ReadValue("type","host",j);
							
							if(HostName != "")
								Srv->ChangeHost(user,HostName);
								
							strlcpy(user->oper,rowresult->GetField("type").c_str(),NICKMAX-1);
							WriteOpers("*** %s (%s@%s) is now an IRC operator of type %s",user->nick,user->ident,user->host,rowresult->GetField("type").c_str());
							WriteServ(user->fd,"381 %s :You are now an IRC operator of type %s",user->nick,rowresult->GetField("type").c_str());
							if(user->modes[UM_OPERATOR])
							{
								user->modes[UM_OPERATOR] = 1;
								WriteServ(user->fd,"MODE %s :+o",user->nick);
								FOREACH_MOD(I_OnOper,OnOper(user,rowresult->GetField("type")));
								AddOper(user);
								FOREACH_MOD(I_OnPostOper,OnPostOper(user,rowresult->GetField("type")));
								log(DEFAULT,"OPER: %s!%s@%s opered as type: %s",user->nick,user->ident,user->host,rowresult->GetField("type").c_str());
							}
								
							break;
						}
					}
				}
				
				DELETE(rowresult);
			}
			else
			{
				// we didn't have a row.
				found = false;
			}
			
			DELETE(rowrequest);
			DELETE(result);
		}
		else
		{
			// the query was bad
			found = false;
		}
		query->SetQueryType(SQL_DONE);
		query->SetConnID(dbid);
		Request donerequest((char*)query, this, SQLModule);
		donerequest.Send();
		DELETE(query);
		return found;
	}

	virtual ~ModuleSQLOper()
	{
		DELETE(Conf);
	}
	
	virtual Version GetVersion()
	{
		return Version(1,0,0,1,VF_VENDOR);
	}
	
};

class ModuleSQLOperFactory : public ModuleFactory
{
 public:
	ModuleSQLOperFactory()
	{
	}
	
	~ModuleSQLOperFactory()
	{
	}
	
	virtual Module * CreateModule(Server* Me)
	{
		return new ModuleSQLOper(Me);
	}
	
};


extern "C" void * init_module( void )
{
	return new ModuleSQLOperFactory;
}
