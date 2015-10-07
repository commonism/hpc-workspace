/*
 *    workspace++
 *
 *    c++ version of workspace utility
 *  a workspace is a temporary directory created in behalf of a user with a limited lifetime.
 *  This version is not DB and configuration compatible with the older version, the DB and
 *    configuration was changed to YAML files.
 *
 *  differences to old workspace version
 *    - usage of YAML file format
 *    - using setuid or capabilities (needs support by filesystem!)
 *    - always moves released workspace away (this change is affecting the user!)
 *
 *  (c) Holger Berger 2013, 2014, 2015
 *
 *  workspace++ is based on workspace by Holger Berger, Thomas Beisel and Martin Hecht
 *
 *  workspace++ is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  workspace++ is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with workspace++.  If not, see <http://www.gnu.org/licenses/>.
 *
 */


#include <unistd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <pwd.h>
#include <sys/wait.h>


#ifndef SETUID
#include <sys/capability.h>
#else
typedef int cap_value_t;
const int CAP_DAC_OVERRIDE = 0;
const int CAP_CHOWN = 1;
#endif

// C++ stuff
#include <iostream>
#include <string>
#include <vector>

// YAML
#include <yaml-cpp/yaml.h>

// BOOST
#include <boost/regex.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/foreach.hpp>
#include <boost/algorithm/string/predicate.hpp>

#define BOOST_FILESYSTEM_VERSION 3
#define BOOST_FILESYSTEM_NO_DEPRECATED
#include <boost/filesystem.hpp>

// LUA
#ifdef LUACALLOUTS
#include <lua.hpp>
#endif

#include "ws.h"
#include "wsdb.h"

namespace fs = boost::filesystem;
namespace po = boost::program_options;
using boost::lexical_cast;

using namespace std;


/*
 * read global and user config and validate parameters
 */
Workspace::Workspace(const whichclient clientcode, const po::variables_map _opt, const int _duration,
                     string _filesystem)
    : opt(_opt), duration(_duration), filesystem(_filesystem)
{

    // set a umask so users can access db files
    umask(0002);

    // read config
    try {
        config = YAML::LoadFile("/etc/ws.conf");
    } catch (YAML::BadFile) {
        cerr << "Error: no config file!" << endl;
        exit(-1);
    }
    db_uid = config["dbuid"].as<int>();
    db_gid = config["dbgid"].as<int>();

    // lower capabilities to minimum
    drop_cap(CAP_DAC_OVERRIDE, CAP_CHOWN, db_uid);
    // read private config
    raise_cap(CAP_DAC_OVERRIDE);


    // read private config
    raise_cap(CAP_DAC_OVERRIDE);
    try {
        userconfig = YAML::LoadFile("ws_private.conf");
    } catch (YAML::BadFile) {
        // we do not care
    }

    // lower again, nothing needed
    lower_cap(CAP_DAC_OVERRIDE, db_uid);

    username = getusername();

    // valide the input  (opt contains name, duration and filesystem as well)
    validate(clientcode, config, userconfig, opt, filesystem, duration, maxextensions, acctcode);
}

/*
 *  create a workspace and its DB entry
 */
void Workspace::allocate(const string name, const bool extensionflag, const int reminder, const string mailaddress, string user_option) {
    string wsdir;
    int extension;
    long expiration;
#ifdef LUACALLOUTS
    // see if we have a prefix callout
    string prefixcallout;
    lua_State* L;
    if(config["workspaces"][filesystem]["prefix_callout"]) {
        prefixcallout = config["workspaces"][filesystem]["prefix_callout"].as<string>();
        L = lua_open();
        luaL_openlibs(L);
        if(luaL_dofile(L, prefixcallout.c_str())) {
            cerr << "Error: prefix callout script does not exist!" << endl;
            prefixcallout = "";
        }
    }
#endif

    // construct db-entry name, special case if called by root with -x and -u, allows overwrite of maxextensions
    string dbfilename;
    if(extensionflag && user_option.length()>0) {
        dbfilename=config["workspaces"][filesystem]["database"].as<string>() + "/"+user_option+"-"+name;
        if(!fs::exists(dbfilename)) {
            cerr << "Error: workspace does not exist, can not be extended!" << endl;
            exit(-1);
        }
    } else {
        if(user_option.length()>0 && (getuid()==0)) {
            dbfilename=config["workspaces"][filesystem]["database"].as<string>() + "/"+user_option+"-"+name;
        } else {
            dbfilename=config["workspaces"][filesystem]["database"].as<string>() + "/"+username+"-"+name;
        }
    }

    // does db entry exist?
    if(fs::exists(dbfilename)) {
        WsDB dbentry(dbfilename);
        wsdir = dbentry.getwsdir();
        extension = dbentry.getextension();
        expiration = dbentry.getexpiration();
        // if it exists, print it, if extension is required, extend it
        if(extensionflag) {
            // we allow a user to specify -u -x together, and to extend a workspace if the has rights on the workspace
            if(user_option.length()>0 && (user_option != username) && (getuid() != 0)) {
                cerr << "Info: you are not owner of the workspace." << endl;
                if(access(wsdir.c_str(), R_OK|W_OK|X_OK)!=0) {
                    cerr << "Info: and you have no permissions to access the workspace, workspace will not be extended." << endl;
                    exit(-1);
                }
            }
            cerr << "Info: extending workspace." << endl;
            long expiration = time(NULL)+duration*24*3600;
            dbentry.use_extension(expiration);
            extension = dbentry.getextension();
        } else {
            cerr << "Info: reusing workspace." << endl;
        }
    } else {
        // if it does not exist, create it
        cerr << "Info: creating workspace." << endl;
        // read the possible spaces for the filesystem
        vector<string> spaces = config["workspaces"][filesystem]["spaces"].as<vector<string> >();
        string prefix = "";

        // the lua function "prefix" gets called as prefix(filesystem, username)
#ifdef LUACALLOUTS
        if(prefixcallout!="") {
            lua_getglobal(L, "prefix");
            lua_pushstring(L, filesystem.c_str() );
            lua_pushstring(L, username.c_str() );
            lua_call(L, 2, 1);
            prefix = string("/")+lua_tostring(L, -1);
            cerr << "Info: prefix=" << prefix << endl;
            lua_pop(L,1);
        }
#endif

        // add some randomness
        srand(time(NULL));
        if (user_option.length()>0 && (user_option != username) && (getuid() != 0)) {
            wsdir = spaces[rand()%spaces.size()]+prefix+"/"+username+"-"+name;
        } else {  // we are root and can change owner!
            wsdir = spaces[rand()%spaces.size()]+prefix+"/"+user_option+"-"+name;
        }

        // make directory and change owner + permissions
        try {
            raise_cap(CAP_DAC_OVERRIDE);
            fs::create_directories(wsdir);
            lower_cap(CAP_DAC_OVERRIDE, db_uid);
        } catch (...) {
            lower_cap(CAP_DAC_OVERRIDE, db_uid);
            cerr << "Error: could not create workspace directory!"  << endl;
            exit(-1);
        }

        raise_cap(CAP_CHOWN);
        if(chown(wsdir.c_str(), getuid(), getgid())) {
            lower_cap(CAP_CHOWN, db_uid);
            cerr << "Error: could not change owner of workspace!" << endl;
            unlink(wsdir.c_str());
            exit(-1);
        }
        lower_cap(CAP_CHOWN, db_uid);

        raise_cap(CAP_DAC_OVERRIDE);
        if(chmod(wsdir.c_str(), S_IRUSR | S_IWUSR | S_IXUSR)) {
            lower_cap(CAP_DAC_OVERRIDE, db_uid);
            cerr << "Error: could not change permissions of workspace!" << endl;
            unlink(wsdir.c_str());
            exit(-1);
        }
        lower_cap(CAP_DAC_OVERRIDE, db_uid);

        extension = maxextensions;
        expiration = time(NULL)+duration*24*3600;
        WsDB dbentry(dbfilename, wsdir, expiration, extension, acctcode, db_uid, db_gid, reminder, mailaddress);
    }
    cout << wsdir << endl;
    cerr << "remaining extensions  : " << extension << endl;
    cerr << "remaining time in days: " << (expiration-time(NULL))/(24*3600) << endl;

}

/*
 * release a workspace by moving workspace and DB entry into trash
 *
 */
void Workspace::release(string name) {
    string wsdir;

    string dbfilename=config["workspaces"][filesystem]["database"].as<string>()+"/"+username+"-"+name;

    // does db entry exist?
    // cout << "file: " << dbfilename << endl;
    if(fs::exists(dbfilename)) {
        WsDB dbentry(dbfilename);
        wsdir = dbentry.getwsdir();

        string timestamp = lexical_cast<string>(time(NULL));

        string dbtargetname = fs::path(dbfilename).parent_path().string() + "/" +
                              config["workspaces"][filesystem]["deleted"].as<string>() +
                              "/" + username + "-" + name + "-" + timestamp;
        // cout << dbfilename.c_str() << "-" << dbtargetname.c_str() << endl;
        raise_cap(CAP_DAC_OVERRIDE);
        if(rename(dbfilename.c_str(), dbtargetname.c_str())) {
            // cerr << "rename " << dbfilename.c_str() << " -> " << dbtargetname.c_str() << " failed" << endl;
            lower_cap(CAP_DAC_OVERRIDE, config["dbuid"].as<int>());
            cerr << "Error: database entry could not be deleted." << endl;
            exit(-1);
        }
        lower_cap(CAP_DAC_OVERRIDE, config["dbuid"].as<int>());

        // rational: we move the workspace into deleted directory and append a timestamp to name
        // as a new workspace could have same name and releasing the new one would lead to a name
        // collision, so the timestamp is kind of generation label attached to a workspace

        string wstargetname = fs::path(wsdir).parent_path().string() + "/" +
                              config["workspaces"][filesystem]["deleted"].as<string>() +
                              "/" + username + "-" + name + "-" + timestamp;

        // cout << wsdir.c_str() << " - " << wstargetname.c_str() << endl;
        raise_cap(CAP_DAC_OVERRIDE);
        if(rename(wsdir.c_str(), wstargetname.c_str())) {
            // cerr << "rename " << wsdir.c_str() << " -> " << wstargetname.c_str() << " failed " << geteuid() << " " << getuid() << endl;

            // fallback to mv for filesystems where rename() of directories returns EXDEV
            int r = mv(wsdir.c_str(), wstargetname.c_str());
            if(r!=0) {
                lower_cap(CAP_DAC_OVERRIDE, config["dbuid"].as<int>());
                cerr << "Error: could not remove workspace!" << endl;
                exit(-1);
            }
        }
        lower_cap(CAP_DAC_OVERRIDE, config["dbuid"].as<int>());

    } else {
        cerr << "Error: workspace does not exist!" << endl;
        exit(-1);
    }

}


/*
 *  validate the commandline versus the configuration file, to see if the user
 *  is allowed to do what he asks for.
 */
void Workspace::validate(const whichclient wc, YAML::Node &config, YAML::Node &userconfig,
                         po::variables_map &opt, string &filesystem, int &duration, int &maxextensions, string &primarygroup)
{

    // get user name, group names etc
    vector<string> groupnames;

    struct group *grp;
    int ngroups = 128;
    gid_t gids[128];
    int nrgroups;

    nrgroups = getgrouplist(username.c_str(), geteuid(), gids, &ngroups);
    if(nrgroups<=0) {
        cerr << "Error: user in too many groups!" << endl;
    }
    for(int i=0; i<nrgroups; i++) {
        grp=getgrgid(gids[i]);
        if(grp) groupnames.push_back(string(grp->gr_name));
    }
    grp=getgrgid(getegid());
    primarygroup=string(grp->gr_name);

    // if the user specifies a filesystem, he must be allowed to use it
    if(opt.count("filesystem")) {
        // check ACLs
        vector<string>user_acl;
        vector<string>group_acl;

        // read ACL lists
        try {
            BOOST_FOREACH(string v,
                          config["workspaces"][opt["filesystem"].as<string>()]["user_acl"].as<vector<string> >())
            user_acl.push_back(v);
        } catch (...) {};
        try {
            BOOST_FOREACH(string v,
                          config["workspaces"][opt["filesystem"].as<string>()]["group_acl"].as<vector<string> >())
            group_acl.push_back(v);
        } catch (...) {};

        // check ACLs
        bool userok=true;
        if(user_acl.size()>0 || group_acl.size()>0) userok=false;
        BOOST_FOREACH(string grp, groupnames) {
            if( find(group_acl.begin(), group_acl.end(), grp) != group_acl.end() ) {
                userok=true;
                break;
            }
        }
        if( find(user_acl.begin(), user_acl.end(), username) != user_acl.end() ) {
            userok=true;
        }
        if(!userok) {
            cerr << "Error: You are not allowed to use the specified workspace!" << endl;
            exit(4);
        }
    } else {
        // no filesystem specified, figure out which to use
        map<string, string>groups_defaults;
        map<string, string>user_defaults;
        BOOST_FOREACH(const YAML::Node &v, config["workspaces"]) {
            try {
                BOOST_FOREACH(string u, config["workspaces."][v.as<string>()]["groupdefault"].as<vector<string> >())
                groups_defaults[u]=v.as<string>();
            } catch (...) {};
            try {
                BOOST_FOREACH(string u, config["workspaces."][v.as<string>()]["userdefault"].as<vector<string> >())
                user_defaults[u]=v.as<string>();
            } catch (...) {};
        }
        if( user_defaults.count(username) > 0 ) {
            filesystem=user_defaults[username];
            goto found;
        }
        if( groups_defaults.count(primarygroup) > 0 ) {
            filesystem=groups_defaults[primarygroup];
            goto found;
        }
        BOOST_FOREACH(string grp, groupnames) {
            if( groups_defaults.count(grp)>0 ) {
                filesystem=groups_defaults[grp];
                goto found;
            }
        }
        // fallback, if no per user or group default, we use the config default
        filesystem=config["default"].as<string>();
found:
        ;
    }

    if(wc==WS_Allocate) {
        // check durations - userexception in workspace/workspace/global
        int configduration;
        if(userconfig["workspaces"][filesystem]["userexceptions"][username]["duration"]) {
            configduration = userconfig["workspaces"][filesystem]["userexceptions"][username]["duration"].as<int>();
        } else {
            if(config["workspaces"][filesystem]["duration"]) {
                configduration = config["workspaces"][filesystem]["duration"].as<int>();
            } else {
                configduration = config["duration"].as<int>();
            }
        }

        // if we are root, we ignore the limits
        if ( getuid()!=0 && opt["duration"].as<int>() > configduration ) {
            duration = configduration;
            cerr << "Error: Duration longer than allowed for this workspace" << endl;
            cerr << "       setting to allowed maximum of " << duration << endl;
        }

        // get extensions from workspace or default  - userexception in workspace/workspace/global
        if(userconfig["workspaces"][filesystem]["userexceptions"][username]["maxextensions"]) {
            maxextensions = userconfig["workspaces"][filesystem]["userexceptions"][username]["maxextensions"].as<int>();
        } else {
            if(config["workspaces"][filesystem]["maxextensions"]) {
                maxextensions = config["workspaces"][filesystem]["maxextensions"].as<int>();
            } else {
                maxextensions = config["maxextensions"].as<int>();
            }
        }
    }
}

/*
 * fallback for rename in case of EXDEV
 * we do not use system() as we are in setuid
 * and it would fail, and it sucks anyhow,
 */
int Workspace::mv(const char * source, const char *target) {
    pid_t pid;
    int status;
    pid = fork();
    if (pid==0) {
        execl("/bin/mv", "mv", source, target, NULL);
    } else if (pid<0) {
        //
    } else {
        waitpid(pid, &status, 0);
        return WEXITSTATUS(status);
    }
    return 0;
}


/*
 * get user name
 * we have this to avoud cuserid
 */
string Workspace::getusername()
{
    struct passwd *pw;

    pw = getpwuid(getuid());
    return string(pw->pw_name);
}

/*
 * get home of current user, we have this to avoid $HOME
 */
string Workspace::getuserhome()
{
    struct passwd *pw;

    pw = getpwuid(getuid());
    return string(pw->pw_dir);
}


/*
 * get filesystem
 */
string Workspace::getfilesystem()
{
    return filesystem;
}


/*
 * get list of restorable workspaces, as names
 */
vector<string> Workspace::getRestorable(string username)
{
    string dbprefix = config["workspaces"][filesystem]["database"].as<string>() + "/" +
                      config["workspaces"][filesystem]["deleted"].as<string>();

    vector<string> namelist;

    fs::directory_iterator end;
    for (fs::directory_iterator it(dbprefix); it!=end; ++it) {
#if BOOST_VERSION < 105000
        if (boost::starts_with(it->path().filename(), username + "-" )) {
            namelist.push_back(it->path().filename());
        }
#else
        if (boost::starts_with(it->path().filename().string(), username + "-" )) {
            namelist.push_back(it->path().filename().string());
        }
#endif
    }

    return namelist;
}

/*
 * restore a workspace, argument is name of workspace DB entry including username and timestamp, form user-name-timestamp
 */
void Workspace::restore(const string name, const string target, const string username) {
    string dbfilename = fs::path(config["workspaces"][filesystem]["database"].as<string>()).string() 
                         + "/" + config["workspaces"][filesystem]["deleted"].as<string>()+"/"+name;

    string targetdbfilename = fs::path(config["workspaces"][filesystem]["database"].as<string>()).string() 
                            + "/" + username + "-" + target;

    string targetwsdir;

    // check for target existance and get directory name of workspace, which will be target of mv operations
    if(fs::exists(targetdbfilename)) {
        WsDB targetdbentry(targetdbfilename);
        targetwsdir = targetdbentry.getwsdir();
    } else {
        cerr << "Error: target workspace does not exist!" << endl;
        exit(1);
    }

    if(fs::exists(dbfilename)) {
        WsDB dbentry(dbfilename);
        // this is path of original workspace, from this we derive the deleted name
        string wsdir = dbentry.getwsdir();

        // go one up, add deleted subdirectory and add workspace name 
        string wssourcename = fs::path(wsdir).parent_path().string() + "/" +
                              config["workspaces"][filesystem]["deleted"].as<string>() +
                              "/" + name;

        raise_cap(CAP_DAC_OVERRIDE);
        mv(wssourcename.c_str(), targetwsdir.c_str());
        unlink(dbfilename.c_str());
        lower_cap(CAP_DAC_OVERRIDE, config["dbuid"].as<int>());
        

    } else {
        cerr << "Error: workspace does not exist." << endl;
    }
}


/*
 * drop effective capabilities, except CAP_DAC_OVERRIDE | CAP_CHOWN
 */
void Workspace::drop_cap(cap_value_t cap_arg, int dbuid)
{
#ifndef SETUID
    cap_t caps;
    cap_value_t cap_list[1];

    cap_list[0] = cap_arg;

    caps = cap_init();

    // cap_list[0] = CAP_DAC_OVERRIDE;
    // cap_list[1] = CAP_CHOWN;

    if (cap_set_flag(caps, CAP_PERMITTED, 1, cap_list, CAP_SET) == -1) {
        cerr << "Error: problem with capabilities." << endl;
    }

    if (cap_set_proc(caps) == -1) {
        cerr << "Error: problem dropping capabilities." << endl;
        cap_t cap = cap_get_proc();
        cerr << "Running with capabilities: " << cap_to_text(cap, NULL) << endl;
        cap_free(cap);
    }

    cap_free(caps);
#else
    // seteuid(0);
    seteuid(dbuid);
#endif
}

void Workspace::drop_cap(cap_value_t cap_arg1, cap_value_t cap_arg2, int dbuid)
{
#ifndef SETUID
    cap_t caps;
    cap_value_t cap_list[2];

    cap_list[0] = cap_arg1;
    cap_list[1] = cap_arg2;

    caps = cap_init();

    // cap_list[0] = CAP_DAC_OVERRIDE;
    // cap_list[1] = CAP_CHOWN;

    if (cap_set_flag(caps, CAP_PERMITTED, 2, cap_list, CAP_SET) == -1) {
        cerr << "Error: problem with capabilities." << endl;
    }

    if (cap_set_proc(caps) == -1) {
        cerr << "Error: problem dropping capabilities." << endl;
        cap_t cap = cap_get_proc();
        cerr << "Running with capabilities: " << cap_to_text(cap, NULL) << endl;
        cap_free(cap);
    }

    cap_free(caps);
#else
    // seteuid(0);
    seteuid(dbuid);
#endif

}

/*
 * remove a capability from the effective set
 */
void Workspace::lower_cap(int cap, int dbuid)
{
#ifndef SETUID
    cap_t caps;
    cap_value_t cap_list[1];

    caps = cap_get_proc();

    cap_list[0] = cap;
    if (cap_set_flag(caps, CAP_EFFECTIVE, 1, cap_list, CAP_CLEAR) == -1) {
        cerr << "Error: problem with capabilities." << endl;
    }

    if (cap_set_proc(caps) == -1) {
        cerr << "Error: problem lowering capabilities." << endl;
        cap_t cap = cap_get_proc();
        cerr << "Running with capabilities: " << cap_to_text(cap, NULL) << endl;
        cap_free(cap);
    }

    cap_free(caps);
#else
    // seteuid(0);
    seteuid(dbuid);
#endif
}

/*
 * add a capability to the effective set
 */
void Workspace::raise_cap(int cap)
{
#ifndef SETUID
    cap_t caps;
    cap_value_t cap_list[1];

    caps = cap_get_proc();

    cap_list[0] = cap;
    if (cap_set_flag(caps, CAP_EFFECTIVE, 1, cap_list, CAP_SET) == -1) {
        cerr << "Error: problem with capabilities." << endl;
    }

    if (cap_set_proc(caps) == -1) {
        cerr << "Error: problem raising capabilities." << endl;
        cap_t cap = cap_get_proc();
        cerr << "Running with capabilities: " << cap_to_text(cap, NULL) << endl;
        cap_free(cap);
    }

    cap_free(caps);
#else
    seteuid(0);
#endif
}
