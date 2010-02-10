/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

/*****************************************************************************
 *
 *  ControlMatcher.cc - Implementation of general purpose matcher
 *
 *
 ****************************************************************************/

#include "ink_config.h"
#include "ink_unused.h"
#include <sys/types.h>
#include "Main.h"
#include "ProxyConfig.h"
#include "ControlMatcher.h"
#include "CacheControl.h"
#include "ParentSelection.h"
#include "HostLookup.h"
#include "HTTP.h"
#include "URL.h"
#include "P_EventSystem.h"
#include "P_Net.h"
#include "P_Cache.h"
#include "P_SplitDNS.h"
#include "congest/Congestion.h"

/****************************************************************
 *   Place all template instantiations at the bottom of the file
 ****************************************************************/



// HttpRequestData accessors
//   Can not be inlined due being virtual functions
//
char *
HttpRequestData::get_string()
{
  char *str = hdr->url_get()->string_get(NULL);
  unescapifyStr(str);
  return str;
}

const char *
HttpRequestData::get_host()
{
  return hostname_str;
}

ip_addr_t
HttpRequestData::get_ip()
{
  return dest_ip;
}

ip_addr_t
HttpRequestData::get_client_ip()
{
  return src_ip;
}

/*************************************************************
 *   Begin class HostMatcher
 *************************************************************/

template<class Data, class Result> HostMatcher<Data, Result>::HostMatcher(const char *name, const char *filename):
data_array(NULL), array_len(-1), num_el(-1), matcher_name(name), file_name(filename)
{
  host_lookup = NEW(new HostLookup(name));
}

template<class Data, class Result> HostMatcher<Data, Result>::~HostMatcher()
{

  delete host_lookup;
  delete[]data_array;
}

//
// template <class Data,class Result>
// void HostMatcher<Data,Result>::Print()
//
//  Debugging Method
//
template<class Data, class Result> void HostMatcher<Data, Result>::Print()
{

  printf("\tHost/Domain Matcher with %d elements\n", num_el);
  host_lookup->Print(PrintFunc);
}

//
// template <class Data,class Result>
// void HostMatcher<Data,Result>::PrintFunc(void* opaque_data)
//
//  Debugging Method
//
template<class Data, class Result> void HostMatcher<Data, Result>::PrintFunc(void *opaque_data)
{
  Data *d = (Data *) opaque_data;
  d->Print();
}

// void HostMatcher<Data,Result>::AllocateSpace(int num_entries)
//
//  Allocates the the HostLeaf and Data arrays
//
template<class Data, class Result> void HostMatcher<Data, Result>::AllocateSpace(int num_entries)
{
  // Should not have been allocated before
  ink_assert(array_len == -1);

  host_lookup->AllocateSpace(num_entries);

  data_array = NEW(new Data[num_entries]);

  array_len = num_entries;
  num_el = 0;
}

// void HostMatcher<Data,Result>::Match(RD* rdata, Result* result)
//
//  Searches our tree and updates argresult for each element matching
//    arg hostname
//
template<class Data, class Result> void HostMatcher<Data, Result>::Match(RD * rdata, Result * result)
{

  void *opaque_ptr;
  Data *data_ptr;
  bool r;

  // Check to see if there is any work to do before makeing
  //   the stirng copy
  if (num_el <= 0) {
    return;
  }

  HostLookupState s;

  r = host_lookup->MatchFirst(rdata->get_host(), &s, &opaque_ptr);

  while (r == true) {
    ink_assert(opaque_ptr != NULL);
    data_ptr = (Data *) opaque_ptr;
    data_ptr->UpdateMatch(result, rdata);

    r = host_lookup->MatchNext(&s, &opaque_ptr);
  }
}

//
// char* HostMatcher<Data,Result>::NewEntry(bool domain_record,
//          char* match_data, char* match_info, int line_num)
//
//   Creates a new host/domain record
//
//   If successful, returns NULL
//   If not, returns a pointer to malloc allocated error string
//     that the caller MUST DEALLOCATE
//
template<class Data, class Result> char *HostMatcher<Data, Result>::NewEntry(matcher_line * line_info)
{

  Data *cur_d;
  char *errBuf;
  char *match_data;

  // Make sure space has been allocated
  ink_assert(num_el >= 0);
  ink_assert(array_len >= 0);

  // Make sure we do not overrun the array;
  ink_assert(num_el < array_len);

  match_data = line_info->line[1][line_info->dest_entry];

  // Make sure that the line_info is not bogus
  ink_assert(line_info->dest_entry < MATCHER_MAX_TOKENS);
  ink_assert(match_data != NULL);

  // Remove our consumed label from the parsed line
  line_info->line[0][line_info->dest_entry - 1] = NULL; // XXX
  line_info->num_el--;

  // Fill in the parameter info
  cur_d = data_array + num_el;
  errBuf = cur_d->Init(line_info);

  if (errBuf != NULL) {
    // There was a problem so undo the effects this function
    memset(cur_d, 0, sizeof(Data));
    return errBuf;
  }
  // Fill in the matching info
  host_lookup->NewEntry(match_data, (line_info->type == MATCH_DOMAIN) ? true : false, cur_d);

  num_el++;
  return NULL;
}

/*************************************************************
 *   End class HostMatcher
 *************************************************************/

//
// RegexMatcher<Data,Result>::RegexMatcher()
//
template<class Data, class Result> RegexMatcher<Data, Result>::RegexMatcher(const char *name, const char *filename):
re_array(NULL), re_str(NULL), data_array(NULL), array_len(-1), num_el(-1), matcher_name(name), file_name(filename)
{
}

//
// RegexMatcher<Data,Result>::~RegexMatcher()
//
template<class Data, class Result> RegexMatcher<Data, Result>::~RegexMatcher()
{
  for (int i = 0; i < num_el; i++) {
    regfree(re_array + i);;
    xfree(re_str[i]);
  }
  delete[]re_str;
  xfree(re_array);
  delete[]data_array;
}

//
// void RegexMatcher<Data,Result>::Print()
//
//   Debugging function
//
template<class Data, class Result> void RegexMatcher<Data, Result>::Print()
{
  printf("\tRegex Matcher with %d elements\n", num_el);
  for (int i = 0; i < num_el; i++) {
    printf("\t\tRegex: %s\n", re_str[i]);
    data_array[i].Print();
  }
}

//
// void RegexMatcher<Data,Result>::AllocateSpace(int num_entries)
//
template<class Data, class Result> void RegexMatcher<Data, Result>::AllocateSpace(int num_entries)
{
  // Should not have been allocated before
  ink_assert(array_len == -1);

  re_array = (regex_t *) xmalloc(sizeof(regex_t) * num_entries);
  memset(re_array, 0, sizeof(regex_t *) * num_entries);

  data_array = NEW(new Data[num_entries]);

  re_str = NEW(new char *[num_entries]);
  memset(re_str, 0, sizeof(char *) * num_entries);

  array_len = num_entries;
  num_el = 0;
}

//
// char* RegexMatcher<Data,Result>::NewEntry(matcher_line* line_info)
//
template<class Data, class Result> char *RegexMatcher<Data, Result>::NewEntry(matcher_line * line_info)
{

  Data *cur_d;
  char *errBuf;
  char *reErr;
  char *pattern;
  int r;

  // Make sure space has been allocated
  ink_assert(num_el >= 0);
  ink_assert(array_len >= 0);

  // Make sure we do not overrun the array;
  ink_assert(num_el < array_len);

  pattern = line_info->line[1][line_info->dest_entry];
  // Make sure that the line_info is not bogus
  ink_assert(line_info->dest_entry < MATCHER_MAX_TOKENS);
  ink_assert(pattern != NULL);

  // Create the compiled regular expression
  r = regcomp(re_array + num_el, pattern, REG_EXTENDED | REG_NOSUB);
  if (r != 0) {
    errBuf = (char *) xmalloc(1024 * sizeof(char));
    reErr = (char *) xmalloc(1024 * sizeof(char));
    *reErr = *errBuf = '\0';
    regerror(r, re_array + num_el, reErr, 1024);
    ink_snprintf(errBuf, 1024, "%s regular expression error at line %d : %s", matcher_name, line_info->line_num, reErr);
    memset(re_array + num_el, 0, sizeof(regex_t));
    xfree(reErr);
    return errBuf;
  }
  re_str[num_el] = xstrdup(pattern);

  // Remove our consumed label from the parsed line
  line_info->line[0][line_info->dest_entry - 1] = NULL; // XXX
  line_info->num_el--;

  // Fill in the parameter info
  cur_d = data_array + num_el;
  errBuf = cur_d->Init(line_info);

  if (errBuf == NULL) {
    num_el++;
  } else {
    // There was a problme so undo the effects this function
    xfree(re_str[num_el]);
    re_str[num_el] = NULL;
    regfree(re_array + num_el);
    memset(re_array + num_el, 0, sizeof(regex_t));
  }

  return errBuf;
}

//
// void RegexMatcher<Data,Result>::Match(RD* rdata, Result* result)
//
//   Coduncts a linear search through the regex array and
//     updates arg result for each regex that matches arg URL
//
template<class Data, class Result> void RegexMatcher<Data, Result>::Match(RD * rdata, Result * result)
{
  char *url_str;
  int r;
  char *errBuf;

  // Check to see there is any work to before we copy the
  //   URL
  if (num_el <= 0) {
    return;
  }

  url_str = rdata->get_string();

  // Can't do a regex match with a NULL string so
  //  use an empty one instead
  if (url_str == NULL) {
    url_str = xstrdup("");
  }
  // INKqa12980
  // The function unescapifyStr() is already called in
  // HttpRequestData::get_string(); therefore, no need to call again here.
  // unescapifyStr(url_str);

  for (int i = 0; i < num_el; i++) {

    r = regexec(re_array + i, url_str, 0, NULL, 0);
    if (r == 0) {
      Debug("matcher", "%s Matched %s with regex at line %d", matcher_name, url_str, data_array[i].line_num);
      data_array[i].UpdateMatch(result, rdata);
    } else if (r != REG_NOMATCH) {
      // An error has occured
      errBuf = (char *) xmalloc(sizeof(char) * 1024);
      *errBuf = '\0';
      regerror(r, NULL, errBuf, 1024);
      Warning("error matching regex at line %d : %s", data_array[i].line_num, errBuf);
      xfree(errBuf);
    }

  }
  xfree(url_str);
}

//
// HostRegexMatcher<Data,Result>::HostRegexMatcher()
//
template<class Data, class Result>
  HostRegexMatcher<Data, Result>::HostRegexMatcher(const char *name, const char *filename)
  :
  RegexMatcher <
  Data,
Result > (name, filename)
{
}

//
// void HostRegexMatcher<Data,Result>::Match(RD* rdata, Result* result)
//
//   Conducts a linear search through the regex array and
//     updates arg result for each regex that matches arg host_regex
//
template<class Data, class Result> void HostRegexMatcher<Data, Result>::Match(RD * rdata, Result * result)
{
  const char *url_str;
  int r;
  char *errBuf;

  // Check to see there is any work to before we copy the
  //   URL
  if (this->num_el <= 0) {
    return;
  }

  url_str = rdata->get_host();

  // Can't do a regex match with a NULL string so
  //  use an empty one instead
  if (url_str == NULL) {
    url_str = "";
  }
  for (int i = 0; i < this->num_el; i++) {

    r = regexec(this->re_array + i, url_str, 0, NULL, 0);
    if (r == 0) {
      Debug("matcher", "%s Matched %s with regex at line %d",
            this->matcher_name, url_str, this->data_array[i].line_num);
      this->data_array[i].UpdateMatch(result, rdata);
    } else if (r != REG_NOMATCH) {
      // An error has occured
      errBuf = (char *) xmalloc(sizeof(char) * 1024);
      *errBuf = '\0';
      regerror(r, NULL, errBuf, 1024);
      Warning("error matching regex at line %d : %s", this->data_array[i].line_num, errBuf);
      xfree(errBuf);
    }
  }
}

//
// IpMatcher<Data,Result>::IpMatcher()
//
template<class Data, class Result> IpMatcher<Data, Result>::IpMatcher(const char *name, const char *filename):
ip_lookup(NULL),
data_array(NULL),
array_len(-1),
num_el(-1),
matcher_name(name),
file_name(filename)
{
}

//
// IpMatcher<Data,Result>::~IpMatcher()
//
template<class Data, class Result> IpMatcher<Data, Result>::~IpMatcher()
{
  delete ip_lookup;
  delete[]data_array;
}

//
// void IpMatcher<Data,Result>::AllocateSpace(int num_entries)
//
template<class Data, class Result> void IpMatcher<Data, Result>::AllocateSpace(int num_entries)
{
  // Should not have been allocated before
  ink_assert(array_len == -1);

  ip_lookup = NEW(new IpLookup(matcher_name));

  data_array = NEW(new Data[num_entries]);

  array_len = num_entries;
  num_el = 0;
}

//
// char* IpMatcher<Data,Result>::NewEntry(matcher_line* line_info)
//
//    Inserts a range the ip lookup table.
//        Creates new table levels as needed
//
//    Returns NULL is all was OK.  On error returns, a malloc
//     allocated error string which the CALLEE is responsible
//     for deallocating
//
template<class Data, class Result> char *IpMatcher<Data, Result>::NewEntry(matcher_line * line_info)
{

  Data *cur_d;
  char *errPtr;
  char *errBuf;
  char *match_data;
  ip_addr_t addr1, addr2;

  // Make sure space has been allocated
  ink_assert(num_el >= 0);
  ink_assert(array_len >= 0);

  // Make sure we do not overrun the array;
  ink_assert(num_el < array_len);

  match_data = line_info->line[1][line_info->dest_entry];

  // Make sure that the line_info is not bogus
  ink_assert(line_info->dest_entry < MATCHER_MAX_TOKENS);
  ink_assert(match_data != NULL);

  // Extract the IP range
  errPtr = ExtractIpRange(match_data, &addr1, &addr2);
  if (errPtr != NULL) {
    const size_t errorSize = 1024;
    errBuf = (char *) xmalloc(errorSize * sizeof(char));
    snprintf(errBuf, errorSize, "%s %s at %s line %d", matcher_name, errPtr, file_name, line_info->line_num);
    return errBuf;
  }

  // Remove our consumed label from the parsed line
  line_info->line[0][line_info->dest_entry] = NULL;
  line_info->num_el--;

  // Fill in the parameter info
  cur_d = data_array + num_el;
  errBuf = cur_d->Init(line_info);
  if (errBuf != NULL) {
    return errBuf;
  }

  ip_lookup->NewEntry(addr1, addr2, cur_d);

  num_el++;
  return NULL;
}

//
// void IpMatcherData,Result>::Match(ip_addr_t addr, RD* rdata, Result* result)
//
template<class Data, class Result>
  void IpMatcher<Data, Result>::Match(ip_addr_t addr, RD * rdata, Result * result)
{
  Data *cur;
  bool found;
  IpLookupState s;

  found = ip_lookup->MatchFirst(addr, &s, (void **) &cur);

  while (found == true) {

    ink_assert(cur != NULL);

    cur->UpdateMatch(result, rdata);

    found = ip_lookup->MatchNext(&s, (void **) &cur);
  }
}


template<class Data, class Result> void IpMatcher<Data, Result>::Print()
{
  printf("\tIp Matcher with %d elements\n", num_el);
  if (ip_lookup != NULL) {
    ip_lookup->Print(IpMatcher<Data, Result>::PrintFunc);
  }
}

template<class Data, class Result> void IpMatcher<Data, Result>::PrintFunc(void *opaque_data)
{
  Data *ptr = (Data *) opaque_data;
  ptr->Print();
}

template<class Data, class Result>
  ControlMatcher<Data, Result>::ControlMatcher(const char *file_var, const char *name,
                                                  const matcher_tags * tags, int flags_in)
{
  char *config_file = NULL;

  flags = flags_in;
  ink_assert(flags & (ALLOW_HOST_TABLE | ALLOW_REGEX_TABLE | ALLOW_IP_TABLE));

  config_tags = tags;
  ink_assert(config_tags != NULL);

  matcher_name = name;
  config_file_var = xstrdup(file_var);
  config_file_path[0] = '\0';

  REC_ReadConfigStringAlloc(config_file, config_file_var);

  if (!(flags & DONT_BUILD_TABLE)) {
    ink_release_assert(config_file != NULL);
    ink_strncpy(config_file_path, system_config_directory, sizeof(config_file_path));
    strncat(config_file_path, DIR_SEP, sizeof(config_file_path) - strlen(config_file_path) - 1);
    strncat(config_file_path, config_file, sizeof(config_file_path) - strlen(config_file_path) - 1);
    xfree(config_file);
  }

  reMatch = NULL;
  hostMatch = NULL;
  ipMatch = NULL;
  hrMatch = NULL;

  if (!(flags & DONT_BUILD_TABLE)) {
    m_numEntries = this->BuildTable();
  } else {
    m_numEntries = 0;
  }
}

template<class Data, class Result> ControlMatcher<Data, Result>::~ControlMatcher()
{
  xfree(config_file_var);

  if (reMatch != NULL) {
    delete reMatch;
  }
  if (hostMatch != NULL) {
    delete hostMatch;
  }
  if (ipMatch != NULL) {
    delete ipMatch;
  }
  if (hrMatch != NULL) {
    delete hrMatch;
  }
}

// void ControlMatcher<Data, Result>::Print()
//
//   Debugging method
//
template<class Data, class Result> void ControlMatcher<Data, Result>::Print()
{
  printf("Control Matcher Table: %s\n", matcher_name);
  if (hostMatch != NULL) {
    hostMatch->Print();
  }
  if (reMatch != NULL) {
    reMatch->Print();
  }
  if (ipMatch != NULL) {
    ipMatch->Print();
  }
  if (hrMatch != NULL) {
    hrMatch->Print();
  }
}


// void ControlMatcher<Data, Result>::Match(RD* rdata
//                                          Result* result)
//
//   Queries each table for the Result*
//
template<class Data, class Result> void ControlMatcher<Data, Result>::Match(RD * rdata, Result * result)
{

  if (hostMatch != NULL) {
    hostMatch->Match(rdata, result);
  }
  if (reMatch != NULL) {
    reMatch->Match(rdata, result);
  }
  if (ipMatch != NULL) {
    ipMatch->Match(rdata->get_ip(), rdata, result);
  }
  if (hrMatch != NULL) {
    hrMatch->Match(rdata, result);
  }
}

int fstat_wrapper(int fd, struct stat *s);

// int ControlMatcher::BuildTable() {
//
//    Reads the cache.config file and build the records array
//      from it
//
template<class Data, class Result> int ControlMatcher<Data, Result>::BuildTableFromString(char *file_buf)
{
  // Table build locals
  Tokenizer bufTok("\n");
  tok_iter_state i_state;
  const char *tmp;
  matcher_line *first = NULL;
  matcher_line *current;
  matcher_line *last = NULL;
  int line_num = 0;
  int second_pass = 0;
  int numEntries = 0;
  bool alarmAlready = false;
  char errBuf[1024];
  char *errPtr = NULL;

  // type counts
  int hostDomain = 0;
  int regex = 0;
  int ip = 0;
  int hostregex = 0;

  if (bufTok.Initialize(file_buf, SHARE_TOKS | ALLOW_EMPTY_TOKS) == 0) {
    // We have an empty file
    return 0;
  }
  // First get the number of entries
  tmp = bufTok.iterFirst(&i_state);
  while (tmp != NULL) {

    line_num++;

    // skip all blank spaces at beginning of line
    while (*tmp && isspace(*tmp)) {
      tmp++;
    }

    if (*tmp != '#' && *tmp != '\0') {

      current = (matcher_line *) xmalloc(sizeof(matcher_line));
      errPtr = parseConfigLine((char *) tmp, current, config_tags);

      if (errPtr != NULL) {
        if (config_tags != &socks_server_tags) {
          snprintf(errBuf, sizeof(errBuf), "%s discarding %s entry at line %d : %s",
                   matcher_name, config_file_path, line_num, errPtr);
          SignalError(errBuf, alarmAlready);
        }
        xfree(current);
      } else {

        // Line parsed ok.  Figure out what the destination
        //  type is and link it into our list
        numEntries++;
        current->line_num = line_num;

        switch (current->type) {
        case MATCH_HOST:
        case MATCH_DOMAIN:
          hostDomain++;
          break;
        case MATCH_IP:
          ip++;
          break;
        case MATCH_REGEX:
          regex++;
          break;
        case MATCH_HOST_REGEX:
          hostregex++;
          break;
        case MATCH_NONE:
        default:
          ink_assert(0);
        }

        if (first == NULL) {
          ink_assert(last == NULL);
          first = last = current;
        } else {
          last->next = current;
          last = current;
        }
      }
    }
    tmp = bufTok.iterNext(&i_state);
  }

  // Make we have something to do before going on
  if (numEntries == 0) {
    xfree(first);
    return 0;
  }
  // Now allocate space for the record pointers
  if ((flags & ALLOW_REGEX_TABLE) && regex > 0) {
    reMatch = NEW((new RegexMatcher<Data, Result> (matcher_name, config_file_path)));
    reMatch->AllocateSpace(regex);
  }

  if ((flags & ALLOW_HOST_TABLE) && hostDomain > 0) {
    hostMatch = NEW((new HostMatcher<Data, Result> (matcher_name, config_file_path)));
    hostMatch->AllocateSpace(hostDomain);
  }

  if ((flags & ALLOW_IP_TABLE) && ip > 0) {
    ipMatch = NEW((new IpMatcher<Data, Result> (matcher_name, config_file_path)));
    ipMatch->AllocateSpace(ip);
  }

  if ((flags & ALLOW_HOST_REGEX_TABLE) && hostregex > 0) {
    hrMatch = NEW((new HostRegexMatcher<Data, Result> (matcher_name, config_file_path)));
    hrMatch->AllocateSpace(hostregex);
  }
  // Traverse the list and build the records table
  current = first;
  while (current != NULL) {
    second_pass++;
    if ((flags & ALLOW_HOST_TABLE) && current->type == MATCH_DOMAIN) {
      errPtr = hostMatch->NewEntry(current);
    } else if ((flags & ALLOW_HOST_TABLE) && current->type == MATCH_HOST) {
      errPtr = hostMatch->NewEntry(current);
    } else if ((flags & ALLOW_REGEX_TABLE) && current->type == MATCH_REGEX) {
      errPtr = reMatch->NewEntry(current);
    } else if ((flags & ALLOW_IP_TABLE) && current->type == MATCH_IP) {
      errPtr = ipMatch->NewEntry(current);
    } else if ((flags & ALLOW_HOST_REGEX_TABLE) && current->type == MATCH_HOST_REGEX) {
      errPtr = hrMatch->NewEntry(current);
    } else {
      errPtr = NULL;
      snprintf(errBuf, sizeof(errBuf), "%s discarding %s entry with unknown type at line %d",
               matcher_name, config_file_path, current->line_num);
      SignalError(errBuf, alarmAlready);
    }

    // Check to see if there was an error in creating
    //   the NewEntry
    if (errPtr != NULL) {
      SignalError(errPtr, alarmAlready);
      xfree(errPtr);
      errPtr = NULL;
    }
    // Deallocate the parsing structure
    last = current;
    current = current->next;
    xfree(last);
  }

  ink_assert(second_pass == numEntries);

  if (is_debug_tag_set("matcher")) {
    Print();
  }
  return numEntries;
}

template<class Data, class Result> int ControlMatcher<Data, Result>::BuildTable()
{

  // File I/O Locals
  char *file_buf;
  int ret;

  file_buf = readIntoBuffer(config_file_path, matcher_name, NULL);

  if (file_buf == NULL) {
    return 1;
  }

  ret = BuildTableFromString(file_buf);
  xfree(file_buf);
  return ret;
}


/****************************************************************
 *    TEMPLATE INSTANTIATIONS GO HERE
 *
 *  We have to explictly instantiate the templates so that
 *   evertything works on with dec ccx, sun CC, and g++
 *
 *  Details on the different comipilers:
 *
 *  dec ccx: Does not seem to instantiate anything automatically
 *         so it needs all templates manually instantiated
 *
 *  sun CC: Automatic instantiation works but since we make
 *         use of the templates in other files, instantiation
 *         only occurs when those files are compiled, breaking
 *         the dependency system.  Explict instantiation
 *         in this file causes the templates to be reinstantiated
 *         when this file changes.
 *
 *         Also, does not give error messages about template
 *           compliation problems.  Requires the -verbose=template
 *           flage to error messages
 *
 *  g++: Requires instantiation to occur in the same file as the
 *         the implementation.  Instantiating ControlMatcher
 *         automatically instatiatiates the other templates since
 *         ControlMatcher makes use of them
 *
 ****************************************************************/

template class ControlMatcher<ParentRecord, ParentResult>;
template class HostMatcher<ParentRecord, ParentResult>;
template class RegexMatcher<ParentRecord, ParentResult>;
template class IpMatcher<ParentRecord, ParentResult>;
template class HostRegexMatcher<ParentRecord, ParentResult>;

#ifndef INK_NO_HOSTDB
template class ControlMatcher<SplitDNSRecord, SplitDNSResult>;
template class HostMatcher<SplitDNSRecord, SplitDNSResult>;
template class RegexMatcher<SplitDNSRecord, SplitDNSResult>;
template class IpMatcher<SplitDNSRecord, SplitDNSResult>;
template class HostRegexMatcher<SplitDNSRecord, SplitDNSResult>;
#endif

#ifndef INK_NO_ACL
template class ControlMatcher<CacheControlRecord, CacheControlResult>;
template class HostMatcher<CacheControlRecord, CacheControlResult>;
template class RegexMatcher<CacheControlRecord, CacheControlResult>;
template class IpMatcher<CacheControlRecord, CacheControlResult>;
#endif

template class ControlMatcher<CongestionControlRecord, CongestionControlRule>;
template class HostMatcher<CongestionControlRecord, CongestionControlRule>;
template class HostRegexMatcher<CongestionControlRecord, CongestionControlRule>;
template class RegexMatcher<CongestionControlRecord, CongestionControlRule>;
template class IpMatcher<CongestionControlRecord, CongestionControlRule>;
