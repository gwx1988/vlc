/*
 * HTTPConnection.cpp
 *****************************************************************************
 * Copyright (C) 2014-2015 - VideoLAN and VLC Authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "HTTPConnection.hpp"
#include "ConnectionParams.hpp"
#include "Sockets.hpp"
#include "../adaptive/tools/Helper.h"

#include <cstdio>
#include <sstream>
#include <vlc_stream.h>

using namespace adaptive::http;

AbstractConnection::AbstractConnection(vlc_object_t *p_object_)
{
    p_object = p_object_;
    available = true;
    bytesRead = 0;
    contentLength = 0;
}

AbstractConnection::~AbstractConnection()
{

}

bool AbstractConnection::prepare(const ConnectionParams &params_)
{
    if (!available)
        return false;
    params = params_;
    available = false;
    return true;
}

size_t AbstractConnection::getContentLength() const
{
    return contentLength;
}

HTTPConnection::HTTPConnection(vlc_object_t *p_object_, Socket *socket_, bool persistent)
    : AbstractConnection( p_object_ )
{
    socket = socket_;
    psz_useragent = var_InheritString(p_object_, "http-user-agent");
    queryOk = false;
    retries = 0;
    connectionClose = !persistent;
    chunked = false;
    chunked_eof = false;
    chunkLength = 0;
}

HTTPConnection::~HTTPConnection()
{
    free(psz_useragent);
    delete socket;
}

bool HTTPConnection::canReuse(const ConnectionParams &params_) const
{
    return ( available &&
             params.getHostname() == params_.getHostname() &&
             params.getScheme() == params_.getScheme() &&
             params.getPort() == params_.getPort() );
}

bool HTTPConnection::connect()
{
    return socket->connect(p_object, params.getHostname().c_str(),
                                     params.getPort());
}

bool HTTPConnection::connected() const
{
    return socket->connected();
}

void HTTPConnection::disconnect()
{
    queryOk = false;
    bytesRead = 0;
    contentLength = 0;
    chunked = false;
    chunkLength = 0;
    bytesRange = BytesRange();
    socket->disconnect();
}

int HTTPConnection::request(const std::string &path, const BytesRange &range)
{
    queryOk = false;
    chunked = false;
    chunked_eof = false;
    chunkLength = 0;

    /* Set new path for this query */
    params.setPath(path);

    msg_Dbg(p_object, "Retrieving %s @%zu", params.getUrl().c_str(),
                       range.isValid() ? range.getStartByte() : 0);

    if(!connected() && ( params.getHostname().empty() || !connect() ))
        return VLC_EGENERIC;

    bytesRange = range;
    if(range.isValid() && range.getEndByte() > 0)
        contentLength = range.getEndByte() - range.getStartByte() + 1;

    std::string header = buildRequestHeader(path);
    if(connectionClose)
        header.append("Connection: close\r\n");
    header.append("\r\n");

    if(!send( header ))
    {
        socket->disconnect();
        if(!connectionClose)
        {
            /* server closed connection pipeline after last req. need new */
            connectionClose = true;
            return request(path, range);
        }
        return VLC_EGENERIC;
    }

    int i_ret = parseReply();
    if(i_ret == VLC_SUCCESS)
    {
        queryOk = true;
    }
    else if(i_ret == VLC_ETIMEOUT) /* redir */
    {
        socket->disconnect();
        if(locationparams.getScheme().empty())
            params.setPath(locationparams.getPath());
        else
            params = locationparams;
        locationparams = ConnectionParams();
    }
    else if(i_ret == VLC_EGENERIC)
    {
        socket->disconnect();
        if(!connectionClose)
        {
            connectionClose = true;
            return request(path, range);
        }
    }

    return i_ret;
}

ssize_t HTTPConnection::read(void *p_buffer, size_t len)
{
    if( !connected() ||
       (!queryOk && bytesRead == 0) )
        return VLC_EGENERIC;

    if(len == 0)
        return VLC_SUCCESS;

    queryOk = false;

    const size_t toRead = (contentLength) ? contentLength - bytesRead : len;
    if (toRead == 0)
        return VLC_SUCCESS;

    if(len > toRead)
        len = toRead;

    ssize_t ret = ( chunked ) ? readChunk(p_buffer, len)
                              : socket->read(p_object, p_buffer, len);
    if(ret >= 0)
        bytesRead += ret;

    if(ret < 0 || (size_t)ret < len || /* set EOF */
       (contentLength == bytesRead && connectionClose))
    {
        socket->disconnect();
        return ret;
    }

    return ret;
}

bool HTTPConnection::send(const std::string &data)
{
    return send(data.c_str(), data.length());
}

bool HTTPConnection::send(const void *buf, size_t size)
{
    return socket->send(p_object, buf, size);
}

int HTTPConnection::parseReply()
{
    std::string statusline = readLine();

    if(statusline.empty())
        return VLC_EGENERIC;

    if (statusline.compare(0, 9, "HTTP/1.1 ")!=0)
    {
        if(statusline.compare(0, 9, "HTTP/1.0 ")!=0)
            return VLC_ENOOBJ;
        else
            connectionClose = true;
    }

    std::istringstream ss(statusline.substr(9));
    ss.imbue(std::locale("C"));
    int replycode;
    ss >> replycode;

    std::string lines;
    for( ;; )
    {
        std::string l = readLine();
        if(l.empty())
            break;
        lines.append(l);

        size_t split = lines.find_first_of(':');
        if(split != std::string::npos)
        {
            size_t value = lines.find_first_not_of(' ', split + 1);
            if(value == std::string::npos)
                value = lines.length();
            onHeader(lines.substr(0, split), lines.substr(value));
            lines = std::string();
        }
    }

    if((replycode == 301 || replycode == 302 || replycode == 307 || replycode == 308) &&
       !locationparams.getUrl().empty())
    {
        msg_Info(p_object, "%d redirection to %s", replycode, locationparams.getUrl().c_str());
        return VLC_ETIMEOUT;
    }
    else if (replycode != 200 && replycode != 206)
    {
        msg_Err(p_object, "Failed reading %s: %s", params.getUrl().c_str(), statusline.c_str());
        return VLC_ENOOBJ;
    }

    return VLC_SUCCESS;
}

ssize_t HTTPConnection::readChunk(void *p_buffer, size_t len)
{
    size_t copied = 0;

    for( ; copied < len && !chunked_eof; )
    {
        /* adapted from access/http/chunked.c */
        if(chunkLength == 0)
        {
            std::string line = readLine();
            int end;
            if (std::sscanf(line.c_str(), "%zx%n", &chunkLength, &end) < 1
                    || (line[end] != '\0' && line[end] != ';' /* ignore extension(s) */))
                return -1;
        }

        if(chunkLength > 0)
        {
            size_t toread = len - copied;
            if(toread > chunkLength)
                toread = chunkLength;

            ssize_t in = socket->read(p_object, &((uint8_t*)p_buffer)[copied], toread);
            if(in < 0)
            {
                return (copied == 0) ? in : copied;
            }
            else if((size_t)in < toread)
            {
               return copied + in;
            }
            copied += in;
            chunkLength -= in;
        }
        else chunked_eof = true;

        if(chunkLength == 0)
        {
            char crlf[2];
            ssize_t in = socket->read(p_object, &crlf, 2);
            if(in < 2 || memcmp(crlf, "\r\n", 2))
                return (copied == 0) ? -1 : copied;
        }
    }

    return copied;
}

std::string HTTPConnection::readLine()
{
    return socket->readline(p_object);
}

void HTTPConnection::setUsed( bool b )
{
    available = !b;
    if(available)
    {
        if(!connectionClose && contentLength == bytesRead )
        {
            queryOk = false;
            bytesRead = 0;
            contentLength = 0;
            bytesRange = BytesRange();
        }
        else  /* We can't resend request if we haven't finished reading */
            disconnect();
    }
}

void HTTPConnection::onHeader(const std::string &key,
                              const std::string &value)
{
    if(key == "Content-Length")
    {
        std::istringstream ss(value);
        ss.imbue(std::locale("C"));
        size_t length;
        ss >> length;
        contentLength = length;
    }
    else if (key == "Connection" && value =="close")
    {
        connectionClose = true;
    }
    else if (key == "Transfer-Encoding" && value == "chunked")
    {
        chunked = true;
    }
    else if(key == "Location")
    {
        locationparams = ConnectionParams( value );
    }
}

std::string HTTPConnection::buildRequestHeader(const std::string &path) const
{
    std::stringstream req;
    req << "GET " << path << " HTTP/1.1\r\n";
    if((params.getScheme() == "http" && params.getPort() != 80) ||
            (params.getScheme() == "https" && params.getPort() != 443))
    {
        req << "Host: " << params.getHostname() << ":" << params.getPort() << "\r\n";
    }
    else
    {
        req << "Host: " << params.getHostname() << "\r\n";
    }
    req << "Cache-Control: no-cache" << "\r\n" <<
           "User-Agent: " << std::string(psz_useragent) << "\r\n";
    req << extraRequestHeaders();
    return req.str();
}

std::string HTTPConnection::extraRequestHeaders() const
{
    std::stringstream ss;
    ss.imbue(std::locale("C"));
    if(bytesRange.isValid())
    {
        ss << "Range: bytes=" << bytesRange.getStartByte() << "-";
        if(bytesRange.getEndByte())
            ss << bytesRange.getEndByte();
        ss << "\r\n";
    }
    return ss.str();
}

StreamUrlConnection::StreamUrlConnection(vlc_object_t *p_object)
    : AbstractConnection(p_object)
{
    p_streamurl = NULL;
    bytesRead = 0;
    contentLength = 0;
}

StreamUrlConnection::~StreamUrlConnection()
{
    reset();
}

void StreamUrlConnection::reset()
{
    if(p_streamurl)
        vlc_stream_Delete(p_streamurl);
    p_streamurl = NULL;
    bytesRead = 0;
    contentLength = 0;
    bytesRange = BytesRange();
}

bool StreamUrlConnection::canReuse(const ConnectionParams &) const
{
    return available;
}

int StreamUrlConnection::request(const std::string &path, const BytesRange &range)
{
    reset();

    /* Set new path for this query */
    params.setPath(path);

    msg_Dbg(p_object, "Retrieving %s @%zu", params.getUrl().c_str(),
                      range.isValid() ? range.getStartByte() : 0);

    p_streamurl = vlc_stream_NewURL(p_object, params.getUrl().c_str());
    if(!p_streamurl)
        return VLC_EGENERIC;

    stream_t *p_chain = vlc_stream_FilterNew( p_streamurl, "inflate" );
    if( p_chain )
        p_streamurl = p_chain;

    if(range.isValid() && range.getEndByte() > 0)
    {
        if(vlc_stream_Seek(p_streamurl, range.getStartByte()) != VLC_SUCCESS)
        {
            vlc_stream_Delete(p_streamurl);
            return VLC_EGENERIC;
        }
        bytesRange = range;
        contentLength = range.getEndByte() - range.getStartByte() + 1;
    }

    int64_t i_size = stream_Size(p_streamurl);
    if(i_size > -1)
    {
        if(!range.isValid() || contentLength > (size_t) i_size)
            contentLength = (size_t) i_size;
    }
    return VLC_SUCCESS;
}

ssize_t StreamUrlConnection::read(void *p_buffer, size_t len)
{
    if( !p_streamurl )
        return VLC_EGENERIC;

    if(len == 0)
        return VLC_SUCCESS;

    const size_t toRead = (contentLength) ? contentLength - bytesRead : len;
    if (toRead == 0)
        return VLC_SUCCESS;

    if(len > toRead)
        len = toRead;

    ssize_t ret = vlc_stream_Read(p_streamurl, p_buffer, len);
    if(ret >= 0)
        bytesRead += ret;

    if(ret < 0 || (size_t)ret < len || /* set EOF */
       contentLength == bytesRead )
    {
        reset();
        return ret;
    }

    return ret;
}

void StreamUrlConnection::setUsed( bool b )
{
    available = !b;
    if(available && contentLength == bytesRead)
       reset();
}

ConnectionFactory::ConnectionFactory()
{
}

ConnectionFactory::~ConnectionFactory()
{
}

AbstractConnection * ConnectionFactory::createConnection(vlc_object_t *p_object,
                                                         const ConnectionParams &params)
{
    if((params.getScheme() != "http" && params.getScheme() != "https") || params.getHostname().empty())
        return NULL;

    const int sockettype = (params.getScheme() == "https") ? TLSSocket::TLS : Socket::REGULAR;
    Socket *socket = (sockettype == TLSSocket::TLS) ? new (std::nothrow) TLSSocket()
                                                    : new (std::nothrow) Socket();
    if(!socket)
        return NULL;

    /* disable pipelined tls until we have ticket/resume session support */
    HTTPConnection *conn = new (std::nothrow)
            HTTPConnection(p_object, socket, sockettype != TLSSocket::TLS);
    if(!conn)
    {
        delete socket;
        return NULL;
    }

    return conn;
}

AbstractConnection * StreamUrlConnectionFactory::createConnection(vlc_object_t *p_object,
                                                                  const ConnectionParams &)
{
    return new (std::nothrow) StreamUrlConnection(p_object);
}
