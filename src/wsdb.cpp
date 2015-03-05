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

#include <string>
#include <fstream>

#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#include <grp.h>
#include <time.h>

#include <yaml-cpp/yaml.h>

#ifndef SETUID
#include <sys/capability.h>
#else
typedef int cap_value_t;
const int CAP_DAC_OVERRIDE = 0;
const int CAP_CHOWN = 1;
#endif

#include "wsdb.h"
#include "ws.h"

using namespace std;


/*
 * write db file and change owner
 */
void WsDB::write_dbfile(const string filename, const string wsdir, const long expiration, const int extensions, 
                    const string acctcode, const int dbuid, const int dbgid, 
                    const int reminder, const string mailaddress ) 
{
    YAML::Node entry;
    entry["workspace"] = wsdir;
    entry["expiration"] = expiration;
    entry["extensions"] = extensions;
    entry["acctcode"] = acctcode;
    entry["reminder"] = reminder;
    entry["mailaddress"] = mailaddress;
    Workspace::raise_cap(CAP_DAC_OVERRIDE);
    ofstream fout(filename.c_str());
    fout << entry;
    fout.close();
    Workspace::lower_cap(CAP_DAC_OVERRIDE, dbuid);
    Workspace::raise_cap(CAP_CHOWN);
    if(chown(filename.c_str(), dbuid, dbgid)) {
        Workspace::lower_cap(CAP_CHOWN, dbuid);
        cerr << "Error: could not change owner of database entry" << endl;
    }
    Workspace::lower_cap(CAP_CHOWN, dbuid);
}

/*
 * read dbfile and return contents
 */
void WsDB::read_dbfile(string filename, string &wsdir, long &expiration, int &extensions, string &acctcode,
                        int reminder, string mailaddress) 
{
    YAML::Node entry = YAML::LoadFile(filename);
    wsdir = entry["workspace"].as<string>();
    expiration = entry["expiration"].as<long>();
    extensions = entry["extensions"].as<int>();
    acctcode = entry["acctcode"].as<string>();
    reminder = entry["reminder"].as<int>();
    mailaddress = entry["mailaddress"].as<string>();
}

