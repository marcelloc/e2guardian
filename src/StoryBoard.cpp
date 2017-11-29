// ListContainer - class for both item and phrase lists

// For all support, instructions and copyright go to:
// http://e2guardian.org/
// Released under the GPL v2, with the OpenSSL exception described in the README file.

// INCLUDES

#ifdef HAVE_CONFIG_H
#include "dgconfig.h"
#endif

#include <syslog.h>
#include <algorithm>
#include <netdb.h> // for gethostby
#include "ListContainer.hpp"
#include "StoryBoard.hpp"
#include "OptionContainer.hpp"
#include "RegExp.hpp"
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <sys/time.h>
#include <list>
#include <vector>

// GLOBALS

extern bool is_daemonised;
extern OptionContainer o;
extern thread_local std::string thread_id;

// DEFINES

// Constructor - set default values
StoryBoard::StoryBoard() {
    fnt_cnt = 0;
}

// delete the memory block when the class is destryed
StoryBoard::~StoryBoard() {
    reset();
}

//  clear & reset all values
void StoryBoard::reset() {
}


bool StoryBoard::readFile(const char *filename, ListMeta &LM, bool is_top) {
    if (strlen(filename) < 3) {
        if (!is_daemonised) {
            std::cerr << "Storyboard file" << filename << " is not defined" << std::endl;
        }
        syslog(LOG_ERR, "Storyboard file %s is not defined", filename);
        return false;
    }
#ifdef DGDEBUG
    std::cerr << "Reading storyboard file " << filename << std::endl;
#endif

    LMeta = &LM;
    std::string linebuffer; // a string line buffer ;)
    String temp; // a String for temporary manipulation
    String line;
    String command;
    String params;
    String action;
    SBFunction curr_function;
    int fnt_id = 0;
    bool overwrite = false;
    bool in_function = false;
    size_t len = 0;
    std::ifstream listfile(filename, std::ios::in); // open the file for reading
    if (!listfile.good()) {
        if (!is_daemonised) {
            std::cerr << "Error opening Storyboard file (does it exist?): " << filename << std::endl;
        }
        syslog(LOG_ERR, "Error opening Storyboard file (does it exist?): %s", filename);
        return false;
    }
    bool caseinsensitive = true;
    unsigned int line_no = 0;
    while (!listfile.eof()) { // keep going until end of file
        getline(listfile, linebuffer); // grab a line
        ++line_no;
        if (linebuffer.length() == 0) { // sanity checking
            continue;
        }
#ifdef DGDEBUG
        std::cerr << "Readline " << linebuffer << std::endl;
#endif
        line = linebuffer.c_str();
        line.removeWhiteSpace();
        if (caseinsensitive)
            line.toLower();
        // handle included list files
        if (line.startsWith(".")) {
            temp = line.after(".include<").before(">");
            if (temp.length() > 0) {
                if (!readFile(temp.toCharArray(), *LMeta, false)) {
                    listfile.close();
                    return false;
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (line.startsWith("#")) continue;   // ignore comment lines
        command = line.before("(");
        command.removeWhiteSpace();
        params = line.before(")").after("(");
        params.removeWhiteSpace();
        action = line.after(")");
        action.removeWhiteSpace();
        if (command == "function") {
            if (in_function) {    // already in another function definition & so assume end of previous function
                curr_function.end();
                // push function to list
                if (overwrite)
                    funct_vec.at(--fnt_id) = curr_function;
                else
                    funct_vec.push_back(curr_function);
            }
            in_function = true;
            String temp = params;
            int oldf = 0;
            if ((oldf = getFunctID(temp)) > 0) {
                if (oldf > SB_BI_FUNC_BASE) {   // overloadng buildin action
                    std::cerr << "SB: error - reserved word used a function name - " << temp.c_str() << std::endl;
                    return false;
                } else {
                    fnt_id = oldf;
                    overwrite = true;
                }
            } else {
                fnt_id = ++fnt_cnt;
                overwrite = false;
            }
            curr_function.start(params, fnt_id, line_no, filename);
            continue;
        }
        if (command == "end") {
            if (in_function) {
                curr_function.end();
                // push function to list
                if (overwrite)
                    funct_vec.at(--fnt_id) = curr_function;
                else
                    funct_vec.push_back(curr_function);
                in_function = false;
            }    // otherwise ignore
            continue;
        }
        if (!curr_function.addline(command, params, action, line_no))
            return false;
    }

    if (in_function) {  // at eof so now end
        curr_function.end();
        // push function to list
        if (overwrite)
            funct_vec.at(--fnt_id) = curr_function;
        else
            funct_vec.push_back(curr_function);
    }
#ifdef DGDEBUG
    std::cerr << "SB read file finished function vect size is " << funct_vec.size() << "is_top " << is_top << std::endl;
#endif

    if (!is_top) return true;

    // only do the 2nd pass once all file(s) have been read
    // in top file now do second pass to record action functions ids in command lines and add list_ids
#ifdef DGDEBUG
    std::cerr << "SB Start 2nd pass checking" << std::endl;
#endif

    for (std::vector<SBFunction>::iterator i = funct_vec.begin(); i != funct_vec.end(); i++) {
        for (std::deque<SBFunction::com_rec>::iterator j = i->comm_dq.begin(); j != i->comm_dq.end(); j++) {
            // check condition
#ifdef DGDEBUG
            std::cerr << "Line " << j->file_lineno << " state is " << j->state << " actionid " << j->action_id
                      << " listname " << j->list_name << " function " << i->name << " id " << i->fn_id << std::endl;
#endif

            if (j->state < SB_STATE_TOPIN) {   // is an *in condition and requires a list
                std::deque<int> types;
                switch (j->state) {
                    case SB_STATE_SITEIN:
                        types = {LIST_TYPE_IPSITE, LIST_TYPE_SITE, LIST_TYPE_REGEXP_BOOL};
                        break;
                    case SB_STATE_URLIN:
                        types = {LIST_TYPE_IPSITE, LIST_TYPE_SITE, LIST_TYPE_URL, LIST_TYPE_REGEXP_BOOL};
                        break;
                    case SB_STATE_SEARCHIN:
                        types = {LIST_TYPE_SEARCH};
                        break;
                    case SB_STATE_EMBEDDEDIN:
                        types = {LIST_TYPE_IPSITE, LIST_TYPE_SITE, LIST_TYPE_URL, LIST_TYPE_REGEXP_BOOL};
                        break;
                    case SB_STATE_REFERERIN:
                        types = {LIST_TYPE_IPSITE, LIST_TYPE_SITE, LIST_TYPE_URL, LIST_TYPE_REGEXP_BOOL};
                        break;
                    case SB_STATE_FULLURLIN:
                        types = {LIST_TYPE_REGEXP_REP};
                        break;
                    case SB_STATE_HEADERIN:
                        types = {LIST_TYPE_REGEXP_REP, LIST_TYPE_REGEXP_BOOL};
                        break;
                    case SB_STATE_CLIENTIN:
                        types = {LIST_TYPE_IP, LIST_TYPE_SITE};
                        break;
                    case SB_STATE_EXTENSIONIN:
                        types = {LIST_TYPE_FILE_EXT};
                        break;
                    case SB_STATE_MIMEIN:
                        types = {LIST_TYPE_MIME};
                        break;
                    case SB_STATE_USERAGENTIN:
                        types = {LIST_TYPE_REGEXP_BOOL};
                        break;
                }
                bool found = false;
                for (std::deque<int>::iterator k = types.begin(); k != types.end(); k++) {
#ifdef DGDEBUG
                                        std::cerr << "SB  list name " << j->list_name << " checking type  " << *k << std::endl;
#endif
                    ListMeta::list_info listi = LMeta->findList(j->list_name, *k);
#ifdef DGDEBUG
                    std::cerr << "list reference " << listi.list_ref << " '" << listi.name << "' found for "
                              << j->list_name << std::endl;
#endif
                    if (listi.name.length()) {
                        j->list_id_dq.push_back(listi);
                        found = true;
                    }
                }
                if (!found) {
                    // warning message
                    std::cerr << "SB warning: Undefined list " << j->list_name << " used at line " << j->file_lineno
                              << " of " << i->file_name << std::endl;
                } else {
#ifdef DGDEBUG
                    std::cerr << j->list_name << " matches " << j->list_id_dq.size() << " types" << std::endl;
#endif
                }

            }
            // check action
            if ((j->action_id = getFunctID(j->action_name)) == 0) {
                // warning message
                std::cerr << "StoryBoard error: Action not defined " << j->action_name << " at line " << j->file_lineno
                          << " of " << i->file_name << std::endl;
            }
#ifdef DGDEBUG
            std::cerr << "Line " << j->file_lineno << " state is " << j->state << " actionid " << j->action_id
        << " listname " << j->list_name << std::endl;
#endif
    }
}
// check for required functions

return true;
}

unsigned int StoryBoard::getFunctID(String &fname) {
unsigned int i = 0;
// check built in functions first
if (!funct_vec.empty()) {
    i = funct_vec[0].getBIFunctID(fname);
    if (i > 0) return i;
    }
    // check StoryBoard defined functions
   // std::cerr << "Looking for function " << fname << std::endl;;
    for (std::vector<SBFunction>::iterator j = funct_vec.begin(); j != funct_vec.end(); j++) {
        if (j->name == fname)
            return j->fn_id;
    }
    return 0;
}


bool StoryBoard::runFunct(String &fname, NaughtyFilter &cm) {
    return runFunct(getFunctID(fname), cm);
}

bool StoryBoard::runFunct(unsigned int fID, NaughtyFilter &cm) {
    --fID;
    SBFunction *F = &(funct_vec[fID]);
    bool action_return = false;

    if(o.SB_trace) {
        String ot = thread_id;
        ot += "SB:Entering ";
        ot += F->getName();
        ot += " line:";
        String ln(F->file_lineno);
        ot += ln;
        ot += " of ";
        ot += F->file_name;
#ifdef DGDEBUG
        std::cerr << ot << std::endl;
#else
        syslog(LOG_INFO, "%s", ot.toCharArray());
#endif
    }

    for (std::deque<SBFunction::com_rec>::iterator i = F->comm_dq.begin(); i != F->comm_dq.end(); i++) {
        bool isListCheck = false;
        bool isMultiListCheck = false;
        bool isHeaderCheck = false;
        bool state_result = false;
        String target;
        String target2;
        String targetful;
        std::deque<url_rec> targetdq;

        switch (i->state) {
            case SB_STATE_SITEIN:
                isListCheck = true;
                target = cm.urldomain;
                target2 = target;
                targetful = cm.url;
                if (has_reverse_hosts(targetdq, cm))
                    isMultiListCheck = true;
                break;
            case SB_STATE_URLIN:
                isListCheck = true;
                target = cm.baseurl;
                target2 = cm.urldomain;
                targetful = cm.url;
                if (has_reverse_hosts(targetdq, cm))
                    isMultiListCheck = true;
                break;
            case SB_STATE_FULLURLIN:
                isListCheck = true;
                target = cm.baseurl;
                target2 = cm.urldomain;
                targetful = cm.url;
                if (has_reverse_hosts(targetdq, cm))
                    isMultiListCheck = true;
                break;
            case SB_STATE_SEARCHIN:
                if (cm.isSearch) {
                    isListCheck = true;
                    target = cm.search_words;
                }
                break;
            case SB_STATE_EMBEDDEDIN:
                if (!cm.deep_urls_checked) {
                    cm.deep_urls = deep_urls(cm.baseurl, cm);
                    if (cm.deep_urls.size() > 0)
                        cm.hasEmbededURL = true;
                }
                if (cm.hasEmbededURL) {
                    isMultiListCheck = true;
                    targetdq = cm.deep_urls;
                    target = "mutli";
                }
                break;
            case SB_STATE_REFERERIN:
                isListCheck = true;
                target = cm.request_header->getReferer();   // needs spliting before??
                target2 = target.getHostname();
                break;
            case SB_STATE_HEADERIN:
                isHeaderCheck = true;
                break;
            case SB_STATE_CLIENTIN:
                isListCheck = true;
                target = cm.clientip;
                target2 = cm.clienthost;
                break;
            case SB_STATE_USERAGENTIN:
                isListCheck = true;
                target = cm.request_header->userAgent();   // needs spliting before??
                target2 = "";
                break;
            case SB_STATE_CONNECT:
                state_result = cm.isconnect;
                break;
            case SB_STATE_GET:
                state_result = (cm.request_header->requestType() == "GET");
                break;
            case SB_STATE_POST:
                state_result = (cm.request_header->requestType() == "POST");
                break;
            case SB_STATE_EXCEPTIONSET:
                state_result = cm.isexception;
                break;
            case SB_STATE_GREYSET:
                state_result = cm.isGrey;
                break;
            case SB_STATE_BLOCKSET:
                state_result = cm.isBlocked;
                break;
            case SB_STATE_MITMSET:
                state_result = cm.ismitm;
                break;
            case SB_STATE_DONESET:
                state_result = cm.isdone;
                break;
            case SB_STATE_RETURNSET:
                state_result = cm.isReturn;
                break;
            case SB_STATE_REDIRECTSET:
                state_result = cm.urlredirect;
                break;
            case SB_STATE_VIRUSCHECKSET:
                state_result = !cm.noviruscheck;
                break;
            case SB_STATE_BYPASSSET:
                state_result = cm.isbypass;
                break;
            case SB_STATE_HASSNI:
                state_result = cm.hasSNI;
                break;
            case SB_STATE_TLS:
                state_result = cm.isTLS;
                break;
            case SB_STATE_SITEISIP:
                state_result = cm.isiphost;
                break;
            case SB_STATE_TRUE:
                state_result = true;
                break;
        }
#ifdef DGDEBUG
        std::cerr << "SB state " << F->getState(i->state) << " target " << target << " target2 " << target2
                  << " state_result " << state_result <<
                  " list_check " << isListCheck << " targetfull " << targetful << " isSearch " << cm.isSearch
                  << std::endl;
#endif

        if (isHeaderCheck) {
            for (std::deque<String>::iterator u = cm.request_header->header.begin();
                 u != cm.request_header->header.end(); u++) {
                String t = *u;
                for (std::deque<ListMeta::list_info>::iterator j = i->list_id_dq.begin();
                     j != i->list_id_dq.end(); j++) {
                    ListMeta::list_result res;
#ifdef DGDEBUG
                    std::cerr << "checking " << j->name << " type " << j->type << std::endl;
#endif
                    if (LMeta->inList(*j, t, res)) {  //found
                        state_result = true;
                        if (i->isif) {
                            cm.lastcategory = res.category;
                            cm.whatIsNaughtyCategories = res.category;
                            cm.message_no = res.mess_no;
                            cm.log_message_no = res.log_mess_no;
                            cm.lastmatch = res.match;
                            cm.result = res.result;
                        }
#ifdef DGDEBUG
                        std::cerr << "SB lc" << cm.lastcategory << " mess_no " << cm.message_no << " log_mess "
                                  << cm.log_message_no << " match " << res.match << std::endl;
#endif
                        break;
                    }
                }
                if(state_result)
                    break;
            }
        }
        if (isListCheck) {
            for (std::deque<ListMeta::list_info>::iterator j = i->list_id_dq.begin(); j != i->list_id_dq.end(); j++) {
                ListMeta::list_result res;
                String t;
                if ((j->type >= LIST_TYPE_SITE) && (j->type < LIST_TYPE_URL)) {
                    t = target2;
                } else if (j->type == LIST_TYPE_REGEXP_BOOL || j->type == LIST_TYPE_REGEXP_REP) {
                    t = targetful;
                } else {
                    t = target;
                }
#ifdef DGDEBUG
                std::cerr << "checking " << j->name << " type " << j->type << std::endl;
#endif
                if (LMeta->inList(*j, t, res)) {  //found
                    state_result = true;
                    if (i->isif) {
                        cm.lastcategory = res.category;
                        cm.whatIsNaughtyCategories = res.category;
                        cm.message_no = res.mess_no;
                        cm.log_message_no = res.log_mess_no;
                        cm.lastmatch = res.match;
                        cm.result = res.result;
                    }

#ifdef DGDEBUG
                    std::cerr << "SB lc" << cm.lastcategory << " mess_no " << cm.message_no << " log_mess "
                              << cm.log_message_no << " match " << res.match << std::endl;
#endif
                    break;
                }
            }
        }
        if (isMultiListCheck && !state_result) {
            for (std::deque<url_rec>::iterator u = targetdq.begin(); u != targetdq.end(); u++) {

                for (std::deque<ListMeta::list_info>::iterator j = i->list_id_dq.begin();
                     j != i->list_id_dq.end(); j++) {
                    ListMeta::list_result res;
                    String t;
                    if ((j->type >= LIST_TYPE_SITE) && (j->type < LIST_TYPE_URL)) {
                        t = u->urldomain;
                    } else if (j->type == LIST_TYPE_REGEXP_BOOL || j->type == LIST_TYPE_REGEXP_REP) {
                        t = u->fullurl;
                    } else {
                        t = u->baseurl;
                    }
                    if (u->is_siteonly && j->type == LIST_TYPE_URL)
                        continue;
                    if (!(u->site_is_ip) && j->type == LIST_TYPE_IPSITE)
                        continue;
                    std::cerr << "checking " << j->name << " type " << j->type << "Target " << t << std::endl;
                    if (LMeta->inList(*j, t, res)) {  //found
                        state_result = true;
                        if (i->isif) {
                            cm.lastcategory = res.category;
                            cm.whatIsNaughtyCategories = res.category;
                            cm.message_no = res.mess_no;
                            cm.log_message_no = res.log_mess_no;
                            cm.lastmatch = res.match;
                            cm.result = res.result;
                        }
#ifdef DGDEBUG
                        std::cerr << "SB lc" << cm.lastcategory << " mess_no " << cm.message_no << " log_mess "
                                  << cm.log_message_no << " match " << res.match << std::endl;
#endif
                        break;
                    }
                }
                if (state_result) break;
            }
        }
        if (!i->isif) {
            state_result = !state_result;
        }
#ifdef DGDEBUG
        std::cerr << "SB state " << F->getState(i->state) << " target " << target << " target2 " << target2
                  << " state_result " << state_result <<
                  " list_check " << isListCheck << " isSearch " << cm.isSearch << std::endl;
#endif
        if(o.SB_trace) {
            String ot = thread_id;
            ot += "SB:";
            String ln(i->file_lineno);
            ot += ln;
            if (i->isif)
                ot += " if(";
            else
                ot += " ifnot(";
            ot += F->getState(i->state);
            ot += ",";
            ot += i->list_name;
            ot += ") is ";
            if (state_result) {
                ot += "true ";
            }
            else ot += "false ";
#ifdef DGDEBUG
            std::cerr << ot << std::endl;
#else
            syslog(LOG_INFO, "%s", ot.toCharArray());
#endif
        }
        if (!state_result) {
            action_return = false;
            cm.isReturn = action_return;
            continue;        // nothing to do so continue to next SB line
        }

        action_return = true;

        if (i->mess_no > 0) cm.message_no = i->mess_no;
        if (i->log_mess_no > 0) cm.log_message_no = i->log_mess_no;

        std::cerr << "lc" << cm.lastcategory << " mess_no " << cm.message_no << " log_mess " << cm.log_message_no
                  << " match " << cm.whatIsNaughty << " actionis " << i->action_id << std::endl;

        if (i->action_id > SB_BI_FUNC_BASE) {     // is built-in action
            switch (i->action_id) {
                case SB_FUNC_SETEXCEPTION:
                    cm.isexception = true;
                    cm.isGrey = false;
                    cm.isBlocked = false;
                    //cm.exceptionreason = o.language_list.getTranslation(cm.message_no);
                    cm.whatIsNaughty = o.language_list.getTranslation(cm.message_no) + cm.lastmatch;
                    if (cm.log_message_no == 0)
                        cm.whatIsNaughtyLog = cm.whatIsNaughty;
                    else
                        cm.whatIsNaughtyLog = o.language_list.getTranslation(cm.log_message_no) + cm.lastmatch;
                    cm.exceptioncat = cm.lastcategory;
                    break;
                case SB_FUNC_SETGREY:
                    cm.isGrey = true;
                    cm.isexception = false;
                    cm.isBlocked = false;
                    break;
                case SB_FUNC_SETBLOCK:
                    cm.isBlocked = true;
                    cm.isGrey = false;
                    cm.isexception = false;
                    cm.whatIsNaughty = o.language_list.getTranslation(cm.message_no) + cm.lastmatch;
                    if (cm.log_message_no == 0)
                        cm.whatIsNaughtyLog = cm.whatIsNaughty;
                    else
                        cm.whatIsNaughtyLog = o.language_list.getTranslation(cm.log_message_no) + cm.lastmatch;
                    cm.whatIsNaughtyCategories = cm.lastcategory;
                    break;
                case SB_FUNC_SETMODURL:
                    cm.urlmodified = true;
                    cm.request_header->setURL(cm.result);
                    cm.url = cm.result;
                    cm.baseurl = cm.url;
                    cm.baseurl.removeWhiteSpace();
                    cm.baseurl.toLower();
                    cm.baseurl.removePTP();
                    cm.logurl = cm.request_header->getLogUrl(false, cm.ismitm);
                    cm.urld = cm.request_header->decode(cm.url);
                    cm.urldomain = cm.url.getHostname();
                    cm.urldomain.toLower();
                    std::cerr << "SB URL modified to " << cm.url << std::endl;
                    break;
                case SB_FUNC_SETLOGCAT:
                    cm.logcategory = true;
                    cm.whatIsNaughty = o.language_list.getTranslation(cm.message_no) + cm.lastmatch;
                    if (cm.log_message_no == 0)
                        cm.whatIsNaughtyLog = cm.whatIsNaughty;
                    else
                        cm.whatIsNaughtyLog = o.language_list.getTranslation(cm.log_message_no) + cm.lastmatch;
                    cm.whatIsNaughtyCategories = cm.lastcategory;
                    break;
                case SB_FUNC_SETREDIRECT:
                    if (cm.result.size() > 0) {
                        cm.request_header->redirect = cm.result;
                        cm.urlredirect = true;
                    } else {
                        action_return = false;
                    }
                    break;
                case SB_FUNC_SETGOMITM:
                    cm.gomitm = true;
                    break;
                case SB_FUNC_SETADDHEADER:
                    cm.headeradded = true;
                    cm.request_header->addHeader(cm.result);
                    break;
                case SB_FUNC_SETMODHEADER:
                    cm.headermodified = true;
                    break;
                case SB_FUNC_SETNOCHECKCERT:
                    cm.nocheckcert = true;
                    break;
                case SB_FUNC_SETSEARCHTERM:
                    if (cm.result.size() > 0) {
                        cm.isSearch = true;
                        cm.search_words = cm.result.sort_search();
                        cm.search_terms = cm.result;
                        cm.search_terms.swapChar('+', ' ');
                    };
                    break;
                case SB_FUNC_SETGODIRECT:
                    cm.isdirect = true;
                    break;
                case SB_FUNC_SETDONE:
                    cm.isdone = true;
                    break;
                case SB_FUNC_SETNOLOG:
                    cm.nolog = true;
                    break;
                case SB_FUNC_UNSETVIRUSCHECK:
                    cm.noviruscheck = true;
                    break;
                case SB_FUNC_UNSETBYPASS:
                    cm.isbypass= false;
                    cm.iscookiebypass = false;
                    cm.isscanbypass = false;
                    cm.isvirusbypass = false;
                    cm.isexception = false;
                    break;
                case SB_FUNC_SETTRUE:
                    break;
                case SB_FUNC_SETFALSE:
                    action_return = false;
                    break;
            }
            if (o.SB_trace) {
                String ot = thread_id;
                ot += "SB:";
                ot += F->getBIFunct(i->action_id);
                if (action_return) {
                    ot += " true";
                    //   ot +=
                } else ot += " false ";
#ifdef DGDEBUG
                std::cerr << ot << std::endl;
#else
                syslog(LOG_INFO, "%s", ot.toCharArray());
#endif
            }
        } else {      // is SB defined function
            if (i->action_id > 0) {
                action_return = runFunct(i->action_id, cm);
                if(o.SB_trace) {
                    String ot = thread_id;
                    ot += "SB:resuming: ";
                    ot += F->name;
#ifdef DGDEBUG
                    std::cerr << ot << std::endl;
#else
                    syslog(LOG_INFO, "%s", ot.toCharArray());
#endif
                }
            }

        }
       cm.isReturn = action_return;
        if (i->return_after_action)
            break;
        if (i->return_after_action_is_true && action_return)
            break;
    }

        if(o.SB_trace) {
            String ot = thread_id;
            ot += "SB:";
            ot += F->getName();
            ot += " returned ";
            if (action_return)
                ot += "true";
            else
                ot += "false";
#ifdef DGDEBUG
            std::cerr << ot << std::endl;
#else
            syslog(LOG_INFO, "%s", ot.toCharArray());
#endif
        }

    return action_return;
}

bool StoryBoard::setEntry(unsigned int index, String fname) {
    entrys[index] = getFunctID(fname);
    if (entrys[index] > 0) {
        return true;
    }
    return false;
};

bool StoryBoard::runFunctEntry(unsigned int index, NaughtyFilter &cm) {
    if (entrys[index] > 0)
        return runFunct(entrys[index], cm);
    else
        return false;
};

std::deque<url_rec> StoryBoard::deep_urls(String &urld, NaughtyFilter &cm) {
    std::deque<url_rec> temp;
    String durl = urld;
    while (durl.contains(":")) {
        durl = durl.after(":");
        while (durl.startsWith(":'") || durl.startsWith("/")) {
            durl.lop();
        }
        if (durl.size() > 0) {
            url_rec t;
            t.baseurl = durl;
            t.baseurl.removePTP();
            t.fullurl = durl;
            if (durl.startsWith("http:") || durl.startsWith("https:"))
                durl = durl.after(":");
            t.urldomain = t.baseurl.getHostname();
            if (t.baseurl == t.urldomain)
                t.is_siteonly = true;
            if (cm.isIPHostnameStrip(t.urldomain))
                t.site_is_ip = true;
            temp.push_back(t);
        }
    }
    return temp;
}

// reverse DNS lookup on IP. be aware that this can return multiple results, unlike a standard lookup.
std::deque<url_rec> StoryBoard::ipToHostname(NaughtyFilter &cm) {
    std::deque<url_rec> result;
    const char *ip = cm.urldomain.c_str();
    String urlp = cm.urld.after("/");
    struct in_addr address, **addrptr;
    if (inet_aton(ip, &address)) { // convert to in_addr
        struct hostent *answer;
        answer = gethostbyaddr((char *) &address, sizeof(address), AF_INET);
        if (answer) { // sucess in reverse dns
            url_rec t;
            t.urldomain = answer->h_name;
            t.baseurl = t.urldomain + "/" + urlp;
            t.fullurl = "http://" + t.baseurl;
            result.push_back(t);
            //for (addrptr = (struct in_addr **)answer->h_addr_list; *addrptr; addrptr++) {
            //  result->push_back(String(inet_ntoa(**addrptr)));
            //}
        }
    }
    return result;
}

bool StoryBoard::has_reverse_hosts(std::deque<url_rec> &urec, NaughtyFilter &cm) {
    if (!(cm.isiphost && o.reverse_lookups))
        return false;
    if (!cm.reverse_checked) {
        cm.reversedURLs = ipToHostname(cm);
        cm.reverse_checked = true;
    }
    if (cm.reversedURLs.size() > 0) {
        urec = cm.reversedURLs;
        return true;
    }
    return false;
}
