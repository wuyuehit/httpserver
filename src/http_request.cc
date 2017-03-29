/*
 * http_request.cc
 *
 *  Created on: Mar 27, 2017
 *      Author: zhanghao
 */

#include <algorithm>
#include <string.h>
#include "util.h"
#include "http_request.h"
#include "log.h"
#include "resource.h"

using std::cout;
using std::endl;

namespace httpserver {

inline int HttpRequest::ProcessFirstLine(char* buf, int start, int end) {
  int pos = start;
  int is, ie; //start and end position of each item

  //method
  TrimSpace(buf, pos, end);
  if (unlikely(pos > end)) return ST_ERROR;
  is = ie = pos;
  ie = FindChar(buf, is, end, ' ');
  assert(ie > is);
  method_ = string(buf, is, ie-is);

  //uri
  pos = ie+1;
  TrimSpace(buf, pos, end);
  if (unlikely(pos > end)) return ST_ERROR;
  is = ie = pos;
  ie = FindChar(buf, is, end, ' ');
  assert(ie > is);
  uri_ = string(buf, is, ie-is);

  //uri
  pos = ie+1;
  TrimSpace(buf, pos, end);
  if (pos > end) return ST_ERROR;
  http_version_ = string(buf, pos, end-pos+1);

  return ST_SUCCESS;
}

inline int HttpRequest::ProcessOneLine(char* buf, int start, int end) {

  TrimSpace(buf, start, end);
  if (unlikely(start > end)) return ST_ERROR;

  int is = start, ie = FindCharToLower(buf, start, end, ':');
  string key = string(buf, is, ie-is);
  ie++;
  TrimSpace(buf, ie, end);
  if (ie > end) return ST_ERROR;
  TrimSpecialReverse(buf, end, ie);
  headers_[key] = string(buf, ie, end-ie+1);
  return ST_SUCCESS;
}

int HttpRequest::ReadAndParse (ClientSocket* socket) {
  char buf[MAX_HEADER_SIZE];
  int nread = socket->Recv(buf, MAX_HEADER_SIZE);
  if (unlikely(nread <= 0)) { //remote has close the socket
     return ST_CLOSED;
  }

  int ls = 0, le = 0; //start and end position of each line
  int pos = 0;
  int linenum = 0;
  while (pos < nread) {
    TrimSpecial(buf, pos, nread-1);
    if (unlikely(pos >= nread)) break;
    ls = le = pos;
    le = FindChar(buf, ls, nread-1, '\n'); //find the line breaker \r\n or \n
    assert(le > ls); //buf[le-1] == '\n' or le == nread, anyway, buf[ls,le-1] forms a line
    le--; //backwards 1 position (le == nread OR buf[le] = '\n')
    pos = le+1;

    TrimSpecialReverse(buf, le, ls);
    if (unlikely(linenum == 0)) {
      if (ProcessFirstLine(buf, ls, le) == ST_ERROR) return ST_ERROR;
    } else {
      if (ProcessOneLine(buf, ls, le) == ST_ERROR) return ST_ERROR;
    }
    linenum++;
  }

  if (headers_.count("connection")) {
    string& value = headers_["connection"];
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    if (value.find("keep-alive") != string::npos) {
      keep_alive_ = true;
    }
  }

//  stringstream ss;
//  int nread = socket->Recv(ss);
//  if (nread <= 0) { //remote has close the socket
//    return ST_CLOSED;
//  }
//
//  string line;
//  int start = 0, end = 0;
//  getline(ss, line);
//  if (line.length() > 0) { //parse the request line: GET /hello.htm HTTP/1.1
//    //parse method
//    if ((end = line.find(' ')) != string::npos) {
//      method_ = line.substr(start, end-start);
//
//      //parse request URI
//      start = end+1;
//      if ((end = line.find(' ', start)) != string::npos) {
//        uri_ = line.substr(start, end-start);
//      }
//
//      //parse http version
//      start = end+1;
//      if ((end = line.find('/', start)) != string::npos) {
//        http_version_ = line.substr(end+1);
//      }
//    }
//  }
//
//  //parse other headers
//  while (getline(ss, line)) {
//    start = 0;
//    if ((end = line.find(':', start)) != string::npos) {
//      start = end+1;
//      while (start < line.length() && line[start] == ' ') start++;
//
//      string key = line.substr(0, end);
//      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
//
//      if (key.compare("connection") == 0) {
//        string value = line.substr(start);
//        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
//        if (value.find("keep-alive") != string::npos) {
//          keep_alive_ = true;
//        }
//      }
//      headers_[key] = line.substr(start);
//    }
//  }

//  LOG(LOG_WARNING, "method = %s, uri = %s, http_version = %s", method_.c_str(), uri_.c_str(), http_version_.c_str());
//  for (auto& it: headers_) {
//    LOG(LOG_WARNING, "%s: %s", it.first.c_str(), it.second.c_str());
//  }

  return ST_SUCCESS;
}


int HttpRequest::Respond (ClientSocket* socket) {
  stringstream ss;
  //string reply = "<html><body><h1>Hello, World!</h1></body></html>";
  char header[MAX_HEADER_SIZE];
  char sbuf[MAX_RESPONSE_SIZE];
  char* buf = sbuf;
  bool heap_buf = false;
  int pos = 0;

  //put the html file at buf[MAX_HEADER_SIZE, ]
  int size = HttpResource::Instance()->Get(uri_, buf+MAX_HEADER_SIZE, MAX_FILE_SIZE);;
  if (size == 0) {  //read file failed
    LOG(LOG_INFO, "%s not found", uri_.c_str());
    status_ = kNotFound;
  } else if (size > MAX_FILE_SIZE) { //file is larger than the buffer
    LOG(LOG_INFO, "found a big file with size %d", size);
    buf = static_cast<char*>(malloc(MAX_HEADER_SIZE+size));
    heap_buf = true;
    size = HttpResource::Instance()->Get(uri_, buf+MAX_HEADER_SIZE, size);
  }

  memcpy(header+pos, kHttpVersion.c_str(), kHttpVersion.length());
  pos += kHttpVersion.length();

  memcpy(header+pos, status_.c_str(), status_.length());
  pos += status_.length();

  memcpy(header+pos, kOtherHeaders.c_str(), kOtherHeaders.length());
  pos += kOtherHeaders.length();

  memcpy(header+pos, kContentLen.c_str(), kContentLen.length());
  pos += kContentLen.length();
  pos += sprintf(header+pos, "%d\r\n\r\n", size); //end of header

  if (size > 0) { //send header+file
    //copy header to the unified buf
    int leading_empty = MAX_HEADER_SIZE - pos;
    memcpy(buf + leading_empty, header, pos);

    int tsize = size + pos;
    if (tsize != socket->Send(buf + leading_empty, tsize)) {
      if(heap_buf) free(buf);
      return ST_ERROR;
    }
  } else { //send header only
    if (pos != socket->Send(header, pos)) {
      if (heap_buf) free(buf);
      return ST_ERROR;
    }
  }
  if(heap_buf) free(buf);
  return ST_SUCCESS;
}

} //end of namespace

