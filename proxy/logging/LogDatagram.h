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



#ifndef LOG_DATAGRAM_H
#define LOG_DATAGRAM_H

#include <stdarg.h>
#include <stdio.h>

#include "libts.h"
#include "LogFormatType.h"
#include "LogBufferSink.h"

class LogSock;
class LogBuffer;
struct LogBufferHeader;
class LogObject;


/*-------------------------------------------------------------------------
  LogDatagram
  -------------------------------------------------------------------------*/

class LogDatagram:public LogBufferSink
{
public:

	enum
	{
		LOG_DATAGRAM_NO_ERROR = 0,
		LOG_DATAGRAM_ERROR
	};

private:
	// User data
	char *m_ip;
	unsigned int m_port;
	// Internal state
	unsigned int m_sock_buffer_length;
	char *m_sock_buffer;
	int m_sock;
	sockaddr_in m_sock_server_addr;
public:
	Link<LogDatagram> link;

public:
	LogDatagram(const char *ip, unsigned int port);
	LogDatagram(const LogDatagram &);
	~LogDatagram();

	int write(LogBuffer * lb);

	bool is_open() const;
	int open_socket();

private:
	void close_socket();

private:
	// -- member functions not allowed --
	LogDatagram();
	LogDatagram & operator=(const LogDatagram &);
};

#endif
