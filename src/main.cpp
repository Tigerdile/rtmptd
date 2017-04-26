/*
 * rtmptd
 *
 * Simple RTMPT Proxy using Civetweb server, Boost Random, and 
 * Thread Building Blocks
 *
 * By sconley 2017 - Released under MIT license
 *
 * MIT License
 *
 * Copyright (c) 2017 Tigerdile
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <string>
#include <chrono>
#include <map>
#include <stdexcept>
#include <iostream>
#include <vector>

#include <unistd.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>

#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int.hpp>

#include <tbb/concurrent_hash_map.h>

#include <CivetServer.h>


using namespace std::chrono;


/**************************************************************************
 *
 * CLASS DEFINITIONS
 *
 *************************************************************************/

/*
 * This class handles interactions with RTMP connections.
 */
class RTMPConnection
{
    public:
        /*
         * These are all used outside the class.  Easier to expose
         * than to wrap them in meaningless functions as they are
         * sometimes by-reference passed into methods (for mg_write
         * writing).
         */

        int             sock;               // Our socket
        uint32_t        session_id;         // RTMP session ID
        char            delay;              // Delay sequence count
        milliseconds    lastAccessTime;     // Last access time

        /*
         * Constructor takes a socket and the session ID.
         *
         * The session ID is required to make cleaning up
         * RTMPConnections easier.
         */
        RTMPConnection(int sock, uint32_t session_id) noexcept
        {
            this->sock = sock;
            this->session_id = session_id;
            this->lastAccessTime = duration_cast<milliseconds> (
                std::chrono::system_clock::now().time_since_epoch()
            );
            this->delay = 1;
            FD_ZERO(&this->myset);
        }

        /*
         * Make sure our socket is properly cleaned up.
         */
        ~RTMPConnection() noexcept
        {
            shutdown(this->sock, 2);
            close(this->sock);
        }

        /*
         * Keep track of how many empties.  This is
         * used for timing back-off purposes.
         */
        void increaseEmpty() noexcept
        {
            if(!(++this->emptyMessages % 10)) {
                this->emptyMessages = 0;

                if(this->delay < 21) {
                    this->delay += 4;
                }
            }

            this->lastAccessTime = duration_cast<milliseconds> (
                std::chrono::system_clock::now().time_since_epoch()
            );
        }

        /*
         * Reset our back-off counter and delay timing.
         */
        void resetEmpty() noexcept
        {
            this->emptyMessages = 0;
            this->delay = 1;

            this->lastAccessTime = duration_cast<milliseconds> (
                std::chrono::system_clock::now().time_since_epoch()
            );
        }

        /*
         * Wrapper for select to see if we have data.
         */
        bool hasData() noexcept
        {
            this->timeout.tv_sec = 0;
            this->timeout.tv_usec = 500;

            FD_SET(this->sock, &this->myset);

            return(select(this->sock+1, &this->myset, NULL, NULL,
                          &this->timeout) > 0);
        }

    private:
        uint32_t        emptyMessages;  // Track of sequential empty messages
        fd_set          myset;          // Keep an fdset handy for select
        struct timeval  timeout;        // and a timeout for select.
};


/*
 * This class centralizes the connection handling to an RTMP
 * server.  It produces RTMPConnections and caches our connection
 * info for the proxy'd stream server.
 */
class RTMPServer
{
    public:
        /*
         * Connect to a given RTMP Server host and port
         *
         * Throws runtime_error if we cannot resolve RTMP host
         * or there is some other address-related problem.
         */
        RTMPServer(const char* host, const char* port)
        {
            struct addrinfo hints;
            int             err;

            this->servinfo = NULL;

            memset(&hints, 0, sizeof(struct addrinfo));
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;

            err = getaddrinfo(host, port, &hints, &this->servinfo);

            if(err) {
                throw std::runtime_error("Could not resolve RTMP host");
            }
        }

        /*
         * Clean up our memory allocation.
         *
         * This does NOT clean up generated RTMPConnections which
         * would probably be a cool addition to this.
         */
        ~RTMPServer() noexcept
        {
            if(this->servinfo) {
                freeaddrinfo(this->servinfo);
            }
        }

        /*
         * Attempts to connect to our RTMP server and creates an
         * RTMPConnection attached to the resulting socket.
         *
         * Consumes a session ID which is produced at this time
         * by open(...) though I suppose might make more sense
         * in this classs.
         *
         * Throws runtime_error if we could not connect to
         * RTMP host for some reason.
         */
        RTMPConnection* connect(uint32_t session_id)
        {
            int sock;
            struct addrinfo *p;

            // Connect
            for(p = servinfo; p; p = p->ai_next) {
                sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);

                if(sock == -1) {
                    continue;
                }

                if(::connect(sock, p->ai_addr, p->ai_addrlen) == -1) {
                    close(sock);
                    continue;
                }

                break;
            }

            // Raise if necessary
            if(!p) {
                throw std::runtime_error("Could not connect to RTMP host!");
            }

            return new RTMPConnection(sock, session_id);
        }

    private:
        struct addrinfo*    servinfo;       // Resolved server info
};


/**************************************************************************
 *
 * GLOBAL VARIABLES
 *
 *************************************************************************/

// This is a concurrent hash map of our sessions, mapping session ID numbers
// to RTMPConnections.  RTMPConnection may be NULL for 'tombstones' we want
// the main loop garbage collector to remove, so do not assume it is set.
typedef tbb::concurrent_hash_map<uint32_t, RTMPConnection*> session_table_t;
static session_table_t sessions;

// Our RTMP server connector!  Global for the connect method mostly
static RTMPServer* server;


/**************************************************************************
 *
 * HANDLERS
 *
 *************************************************************************/


/*
 * This is a generic error handler just to send a 500
 */
static int send_error_response(struct mg_connection *conn)
{
    mg_printf(conn,
              "HTTP/1.1 500 SERVER ERROR\r\n"
              "Connection: Close\r\n"
              "Content-Length: 0\r\n"
              "Cache-Control: no-cache\r\n\r\n");

    // The docs says this is not valid, but my reading of code
    // suggests that it is.
    mg_close_connection(conn);

    // TODO: clean up session here

    return 500;
}

/*
 * Get an RTMPConnection from a mg_connection, or NULL on error.
 * Will set mg_cry accordingly -- just send 500 on NULL
 */
static RTMPConnection* get_rtmp_from_session(struct mg_connection* conn)
{
    uint32_t                            session_id;
    const struct mg_request_info*       req;
    RTMPConnection*                     con;

    req = mg_get_request_info(conn);

    // If this is null, weird
    if((!req) || (!req->local_uri)) {
        mg_cry(conn,
               "Got a null req when trying to locate session .. what do I do?");
        return NULL;
    }

    // parse out our session ID -- C++ strings makes life easier.
    std::string whole_uri = req->local_uri;

    if(whole_uri.size() < 7) {
        mg_cry(conn, "Got a send without session ID: %s", req->local_uri);
        return NULL;
    }

    std::string id;

    // This is 'fragile', but it will 500 on people monkeying with the URL
    // which is fine by me.
    if(req->local_uri[5] == '/') { // open, idle, send
        id = whole_uri.substr(6);
    } else { // close
        id = whole_uri.substr(7);
    }

    // Convert to uint -- stoul doesn't care about trailing slash, etc.
    try {
        session_id = std::stoul(id);
    } catch(std::invalid_argument& e) {
        mg_cry(conn, "Got invalid session: %s (%s)", id.c_str(), e.what());
        return NULL;
    }

    // Make sure we have it
    {
        session_table_t::const_accessor access;

        if((!sessions.find(access, session_id)) || (!access->second)) {
            mg_cry(conn, "Unknown session ID: %s", id.c_str());
            return NULL;
        }

        con = access->second;
        access.release();
    }

    return con;
}

/*
 * The FCS handler is required for RTMPT.  This just always sends
 * a 200 as its largely ignored.
 */
static int fcs_ident_handler(struct mg_connection *conn, void *ignored)
{
    // This is totally ignored
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Connection: Close\r\n"
              "Content-Length: 0\r\n"
              "Cache-Control: no-cache\r\n\r\n");

    return 200;
}

/*
 * The RTMPT open handler that starts a session.
 */
static int open_handler(struct mg_connection *conn, void *ignored)
{
    uint32_t        session_id;
    int             err;
    RTMPConnection* con;

    // Generate session ID
    boost::random::mt19937 rng;
    boost::random::uniform_int_distribution<uint32_t> range =
        boost::random::uniform_int_distribution<uint32_t>();

    // Make sure it is unique.
    while(1) {
        session_id = range(rng);
        session_table_t::accessor access;

        if(!sessions.find(access, session_id)) {
            // Add in a null to keep the slot open in case of concurrent access
            sessions.insert(access, session_id);
            access->second = NULL;
            access.release();
            break;
        }
    }

    // Make our session
    try {
        con = server->connect(session_id);
    } catch(std::runtime_error& e) {
        // Main loop will clean up our session NULL.
        mg_cry(conn, e.what());
        return send_error_response(conn);
    }

    // Push it into our map.
    {
        session_table_t::accessor access;
        sessions.insert(access, session_id);
        access->second = con;
        access.release();
    }

    // Convert integer to string
    std::string id = std::to_string(session_id);

    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/x-fcs\r\n"
              "Connection: Close\r\n" // TODO: Keepalive?
              "Cache-Control: no-cache\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: %lu\r\n\r\n",
              id.size()+1); // add newline

    mg_write(conn, id.c_str(), id.size());
    mg_write(conn, "\n", 1);

    return 200;
}

/*
 * Handle an idle request (waiting for data)
 */
static int idle_handler(struct mg_connection *conn, void *ignored)
{
    // Started this out using a C++ vector but it was causing frame
    // errors -- not sure why.  Maybe my awkward mix of C and C++.
    // Switched to using a regular ole C buffer.
    RTMPConnection*                     con;
    char                                buf[16384];
    int                                 size;
    size_t                              total_read = 0;
    char*                               superBuf = NULL;
    size_t                              superSize = 0;

    con = get_rtmp_from_session(conn);

    if(!con) {
        return send_error_response(conn);
    }

    while(con->hasData()) {
        // read
        size = recv(con->sock, buf, 16383, 0);

        if(size == 0) {
            // show's over, folks
            mg_cry(conn, "Remote connection closed");
            return send_error_response(conn);
        } else if(size < 0) {
            // show's still over, folks.
            mg_cry(conn, "Remote connection error");
            return send_error_response(conn);
        }

        total_read += size;

        if(size) {
            // TODO: reuse buffers
            superBuf = (char*)realloc(superBuf, superSize + size);
            memcpy(&superBuf[superSize], buf, size);
            superSize += size;
        }
    }

    // Book keeping
    if(total_read) {
        con->resetEmpty();
    } else {
        con->increaseEmpty();
    }

    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/x-fcs\r\n"
              "Connection: Close\r\n" // TODO: Keepalive?
              "Cache-Control: no-cache\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: %lu\r\n\r\n",
              superSize+1);

    mg_write(conn, &con->delay, 1);
    mg_write(conn, superBuf, superSize);

    free(superBuf);

    return 200;
}

/*
 * Handle the send request, which is how the client sends
 * data over the socket
 */
static int send_handler(struct mg_connection *conn, void *ignored)
{
    RTMPConnection*                     con;
    char                                buf[8192];
    size_t                              size;

    con = get_rtmp_from_session(conn);

    if(!con) {
        return send_error_response(conn);
    }

    // Get data and push to RTMP
    while((size = mg_read(conn, buf, 8192)) > 0) {
        // Send it
        int total = 0;
        int bytesleft = size;
        int n;

        // Send it all.
        while(total < size) {
            n = send(con->sock, buf+total, bytesleft, 0);

            if(n == -1) {
                mg_cry(conn, "Lost connection while sending!");
                return send_error_response(conn);
            }

            total += n;
            bytesleft -= n;
        }
    }

    // Route to idle handler after data is sent.  Handling is
    // the same from that point.
    return idle_handler(conn, ignored);
}

/*
 * Close the handler
 */
static int close_handler(struct mg_connection *conn, void *ignored)
{
    RTMPConnection* con;

    con = get_rtmp_from_session(conn);

    if(!con) {
        return send_error_response(conn);
    }

    sessions.erase(con->session_id);
    delete con;

    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/x-fcs\r\n"
              "Connection: Close\r\n"
              "Cache-Control: no-cache\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: 1\r\n\r\n"
    );

    mg_write(conn, "\0", 1);

    // The docs says this is not valid, but my reading of code
    // suggests that it is.
    mg_close_connection(conn);

    return 200;
}

/*
 * Main
 *
 * Arguments, in order:
 *
 * * RTMP Server Host (required)
 * * RTMP Server Port (required)
 * * Number of threads (default 50)
 * * listening_ports setting (default 8080)
 * * error_log_file (default error.txt)
 */
int main(int argc, char** argv, char** envp)
{
    // Settings
    const char*       num_threads = "50";
    const char*       listening_ports = "8080";
    const char*       error_log_file = "error.txt";
    const char*       options[11];

    // server context
    struct mg_context   *ctx;

    // Parse arguments
    switch(argc) {
        case 6:
            error_log_file = argv[5];
        case 5:
            listening_ports = argv[4];
        case 4:
            num_threads = argv[3];
        case 3:
            break;
        default:
            std::cerr << "Syntax: " << argv[0]
                                    << " RemoteServerHost"
                                    << " RemoteServerPort"
                                    << " [num threads]"
                                    << " [listening_ports]"
                                    << " [error log file]"
                                    << std::endl;
            return -1;
    }

    // Stack up our options
    // Yes, I'm aware this is a dumb way to do it.
    options[0] = "enable_keep_alive";
    options[1] = "yes";
    options[2] = "keep_alive_timeout_ms";
    options[3] = "500";
    options[4] = "listening_ports";
    options[5] = listening_ports;
    options[6] = "num_threads";
    options[7] = num_threads;
    options[8] = "error_log_file";
    options[9] = error_log_file;
    options[10] = NULL;

    // init library - use IPv6
    mg_init_library(8);

    // start server
    ctx = mg_start(NULL, NULL, options);

    // try to connect to server.
    try {
        server = new RTMPServer(argv[1], argv[2]);
    } catch(std::runtime_error& e) {
        // Probably couldn't resolve host.
        std::cerr << e.what();
        mg_stop(ctx);
        mg_exit_library();
        return (int) -1;
    }

    // add request handlers
    mg_set_request_handler(ctx, "/fcs/ident2", fcs_ident_handler,
                           NULL);
    mg_set_request_handler(ctx, "/open", open_handler, NULL);
    mg_set_request_handler(ctx, "/send", send_handler, NULL);
    mg_set_request_handler(ctx, "/idle", idle_handler, NULL);
    mg_set_request_handler(ctx, "/close", close_handler, NULL);


    while(1) {
        sleep(60);
        // TODO: add session cleanup ?
    }

    // Code will never get here.  But in case we ever want a legit
    // shutdown call, this is what we'd do.

    // Stop server
    mg_stop(ctx);

    // Un-init
    mg_exit_library();
    return (int) 0;
}
