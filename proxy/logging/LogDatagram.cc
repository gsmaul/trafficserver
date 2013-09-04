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

/***************************************************************************
 LogDatagram.cc
 ***************************************************************************/

#include "libts.h"

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "Error.h"

#include "P_EventSystem.h"
#include "I_Machine.h"
#include "LogSock.h"

#include "LogField.h"
#include "LogFilter.h"
#include "LogFormat.h"
#include "LogBuffer.h"
#include "LogFile.h"
#include "LogDatagram.h"
#include "LogHost.h"
#include "LogObject.h"
#include "LogUtils.h"
#include "LogConfig.h"
#include "Log.h"

/*-------------------------------------------------------------------------
  LogDatagram::LogDatagram

  This is the common way to create a new LogDatagram object.
  -------------------------------------------------------------------------*/

LogDatagram::LogDatagram(const char *ip, unsigned int port)
{
	Debug("log-datagram", "entering LogDatagram constructor, this=%p", this);

	m_ip = ats_strdup(ip);
	m_port = port;

	m_sock_buffer_length = 0;
	m_sock_buffer = NULL;
	m_sock = -1;

	open_socket();

	Debug("log-datagram", "exiting LogDatagram constructor, this=%p", this);
}

/*-------------------------------------------------------------------------
  LogDatagram::LogDatagram

  This (copy) contructor builds a LogDatagram object from another LogDatagram object.
  -------------------------------------------------------------------------*/

LogDatagram::LogDatagram (const LogDatagram& copy)
{
	Debug("log-datagram", "entering LogDatagram copy constructor, this=%p", this);

	m_ip = ats_strdup(copy.m_ip);
	m_port = copy.m_port;

	m_sock_buffer_length = 0;
	m_sock_buffer = NULL;
	m_sock = -1;

	if (copy.is_open())
	{
		open_socket();
	}

    Debug("log-datagram", "exiting LogDatagram copy constructor, this=%p", this);
}

/*-------------------------------------------------------------------------
  LogDatagram::~LogDatagram
  -------------------------------------------------------------------------*/

LogDatagram::~LogDatagram()
{
	Debug("log-datagram", "entering LogDatagram destructor, this=%p", this);

	close_socket();
	ats_free(m_ip);

	Debug("log-datagram", "exiting LogDatagram destructor, this=%p", this);
}

/*-------------------------------------------------------------------------
  LogDatagram::write
  -------------------------------------------------------------------------*/

int
LogDatagram::write(LogBuffer * lb)
{
	if (!is_open())
	{
		//Debug("log-datagram", "Cannot write LogBuffer to LogDatagram; socket is not open.");
		Note("Cannot write LogBuffer to LogDatagram; socket is not open.");
		return -1;
	}

	if (lb == NULL)
	{
		//Debug("log-datagram", "Cannot write LogBuffer to LogDatagram; LogBuffer is NULL.");
		Note("Cannot write LogBuffer to LogDatagram; LogBuffer is NULL.");
		return -1;
	}

	LogBufferHeader *buffer_header = lb->header();
	if (buffer_header == NULL)
	{
		Debug("log-datagram", "Cannot write LogBuffer to LogDatagram; LogBufferHeader is NULL.");
		//Note("Cannot write LogBuffer to LogDatagram; LogBufferHeader is NULL.");
		return -1;
	}

	if (buffer_header->entry_count == 0)
	{
		//Debug("log-datagram", "LogBuffer with 0 entries for LogDatagram, nothing to write.");
		Note("LogBuffer with 0 entries for LogDatagram, nothing to write.");
		return 0;
	}

	if (buffer_header->version != LOG_SEGMENT_VERSION)
	{
		//Debug("log-datagram", "Invalid LogBuffer version %d in LogDatagram::write; current version is %d", buffer_header->version, LOG_SEGMENT_VERSION);
		Note("Invalid LogBuffer version %d in LogDatagram::write; current version is %d", buffer_header->version, LOG_SEGMENT_VERSION);
		return 0;
	}

	int bytes_sent = 0;

	char *fieldlist_str = buffer_header->fmt_fieldlist();
	char *printf_str = buffer_header->fmt_printf();

	LogBufferIterator iter(buffer_header);
	LogEntryHeader *entry_header;

	while ((entry_header = iter.next()))
	{
		// Format packet
		int sock_buffer_length = (m_sock_buffer_length - 1);

		m_sock_buffer[0] = 0;
		m_sock_buffer[sock_buffer_length] = 0;

		int sock_buffer_bytes = LogBuffer::to_ascii(
			entry_header,
			(LogFormatType)buffer_header->format_type,
			m_sock_buffer,
			sock_buffer_length,
			fieldlist_str,
			printf_str,
			buffer_header->version,
			NULL
		);

		// sock_buffer_bytes = sock_buffer_length;
		// for (int i = 0; i < sock_buffer_length; i++)
		// {
		// 	m_sock_buffer[i] = ('a' + (char)(i % 26));
		// }

		// Send packet
		int send_res = sendto(m_sock, (const char *)m_sock_buffer, sock_buffer_bytes, 0, (const sockaddr *)&m_sock_server_addr, sizeof(m_sock_server_addr));

		if (send_res >= 0)
		{
			bytes_sent += send_res;

			//Debug("log-datagram", "LogDatagram::write sent %d bytes", send_res);
		}
		else
		{
			//Debug("log-datagram", "LogDatagram::write failed: %d, %s", (int)errno, strerror(errno));
			Error("LogDatagram::write failed: %d, %s", (int)errno, strerror(errno));
		}
	}

	return bytes_sent;
}

/*-------------------------------------------------------------------------
  LogDatagram::is_open
  -------------------------------------------------------------------------*/

bool
LogDatagram::is_open() const
{
	return (bool)(m_sock != -1);
}

/*-------------------------------------------------------------------------
  LogDatagram::open_socket
  -------------------------------------------------------------------------*/

int
LogDatagram::open_socket()
{
	if (is_open())
	{
		return LOG_DATAGRAM_NO_ERROR;
	}

	Debug("log-datagram", "LogDatagram opening %s:%u ...", m_ip, m_port);

	// Open socket
	m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m_sock < 0)
	{
		m_sock = -1;
		Error("Error creating datagram socket %s.", strerror(errno));
		return LOG_DATAGRAM_ERROR;
	}

	// Set options
	int on = 1;
	setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));

	// Bind to socket
	sockaddr_in bindAddr;

	memset((char *)&bindAddr, 0, sizeof(bindAddr));
	bindAddr.sin_family = AF_INET;
	bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	bindAddr.sin_port = htons(0);

	if (bind(m_sock, (struct sockaddr *)&bindAddr, sizeof(bindAddr)) < 0)
	{
		close(m_sock);
		m_sock = -1;
		Error("Error binding datagram socket %s.", strerror(errno));
		return LOG_DATAGRAM_ERROR;
	}

	// Setup server address
	m_sock_server_addr.sin_family = AF_INET;
	m_sock_server_addr.sin_port = htons(m_port);

	if (inet_pton(AF_INET, m_ip, &(m_sock_server_addr.sin_addr)) != 1)
	{
		close(m_sock);
		m_sock = -1;
		Error("Error converting datagram send address.");
		return LOG_DATAGRAM_ERROR;
	}

	// Create send buffer
	m_sock_buffer_length = 1400;
	m_sock_buffer = NEW(new char[m_sock_buffer_length + 1]);
	m_sock_buffer[0] = 0;

	Debug("log-datagram", "LogDatagram %s:%u is open.", m_ip, m_port);

	return LOG_DATAGRAM_NO_ERROR;
}

/*-------------------------------------------------------------------------
  LogDatagram::close_socket
  -------------------------------------------------------------------------*/

void
LogDatagram::close_socket()
{
	if (is_open())
	{
		m_sock_buffer_length = 0;

		delete[] m_sock_buffer;
		m_sock_buffer = NULL;

		close(m_sock);
		m_sock = -1;

		Debug("log-datagram", "LogDatagram %s:%u is closed.", m_ip, m_port);
	}
}

