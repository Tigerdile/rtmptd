/*
 * rtmptd
 *
 * Simple RTMPT Proxy using Civetweb server and Boost Random
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
#include <cstring>
#include <chrono>
#include <unordered_map>
#include <stdexcept>
#include <iostream>
#include <vector>
#include <mutex>

#include <unistd.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>
#include <grp.h>

#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int.hpp>

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
        uint64_t        session_id;         // RTMP session ID
        char            delay;              // Delay sequence count
        milliseconds    lastAccessTime;     // Last access time

        /*
         * Constructor takes a socket and the session ID.
         *
         * The session ID is required to make cleaning up
         * RTMPConnections easier.
         */
        RTMPConnection(int sock, uint64_t session_id) noexcept
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
         */
        ~RTMPServer() noexcept
        {
            if(this->servinfo) {
                freeaddrinfo(this->servinfo);
            }

            for(auto it : this->sessions) {
                if(it.second) {
                    delete it.second;
                }
            }
        }

        /*
         * Generate a session ID
         *
         * The sessionID should be unique for a given second.  If
         * it isn't ... wow, life sucks, but someone will have to
         * re-load their client.
         */
        uint64_t createSessionId() noexcept
        {
            // Generate session ID
            uint64_t                session_id;
            uint32_t                now;
            boost::random::mt19937  rng;
            boost::random::uniform_int_distribution<uint32_t> range =
                boost::random::uniform_int_distribution<uint32_t>();

            // Make the high bytes the system time, and the low
            // bytes will be a 32 bit random number.
            now = time(NULL);

            memcpy(&session_id, &now, 4);

            // Make sure it is unique.
            while(1) {
                now = range(rng);

                // set the random number to the higher bytes
                memcpy((&session_id)+4, &now, 4);

                // Check it
                if(!this->sessions.count(session_id)) {
                    break;
                }
            }

            return session_id;
        }

        /*
         * Attempts to connect to our RTMP server and creates an
         * RTMPConnection attached to the resulting socket.  Returns
         * a session ID.
         *
         * Throws runtime_error if we could not connect to
         * RTMP host for some reason.
         */
        uint64_t connect()
        {
            int sock;
            struct addrinfo *p;
            RTMPConnection  *con;

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

            // Allocate a session and add it to our list.
            this->mtx.lock();
            con = new RTMPConnection(sock, this->createSessionId());
            this->sessions[con->session_id] = con;
            this->mtx.unlock();

            return con->session_id;
        }

        /*
         * Check out a connection.  This will ensure we don't clean it up
         * while it's in use, but please return it (or delete it!)
         *
         * Returns NULL if no session, which is probably an error.
         */
        RTMPConnection* checkout(uint64_t session_id) noexcept
        {
            RTMPConnection  *con = NULL;

            this->mtx.lock();

            try {
                con = this->sessions.at(session_id);
                this->sessions.erase(session_id);
            } catch(std::out_of_range& e) {
            }

            this->mtx.unlock();
            return con;
        }

        /*
         * Check out a connection by parsing the information out of
         * an mg_connection structure.  Uses mg_cry to set error
         * messages -- if a NULL is returned, just 500 and go.
         */
        RTMPConnection* checkout(struct mg_connection* conn)
        {
            const struct mg_request_info*       req;
            std::string                         whole_uri;
            std::string                         id;
            uint64_t                            session_id;

            req = mg_get_request_info(conn);

            // This probably shouldn't be null, but it might be, so check it
            if((!req) || (!req->local_uri)) {
                mg_cry(
                 conn,
                 "Got a null req when trying to locate session .. what do I do?"
                );
                return NULL;
            }

            // parse out our session ID -- C++ strings makes life easier.
            whole_uri = req->local_uri;

            if(whole_uri.size() < 7) {
                mg_cry(conn, "Got a send without session ID: %s", req->local_uri);
                return NULL;
            }

            /*
             * Parse out the ID.  Its a little fragile in that if the URL does not
             * precisely conform to spec, we won't be able to extract the session ID
             * That being said -- we really don't care if a malformed URL doesn't
             * work out right, they should 500 anyway.
             */
            if(req->local_uri[5] == '/') { // open, idle, send
                id = whole_uri.substr(6);
            } else { // close
                id = whole_uri.substr(7);
            }

            // parse it... stoull shouldn't care about trailing slash, etc.
            try {
                session_id = std::stoull(id);
            } catch(std::invalid_argument& e) {
                mg_cry(conn, "Got invalid session: %s (%s)", id.c_str(), e.what());
                return NULL;
            }

            // Route it to regular checkout
            return this->checkout(session_id);
        }

        /*
         * Return a RTMPConnection to the list.
         */
        void checkin(RTMPConnection* con) noexcept
        {
            this->mtx.lock();
            this->sessions[con->session_id] = con;
            this->mtx.unlock();
        }

        /*
         * Runs cleanup, rolls through the list and removes any stale
         * RTMP connections.
         *
         * This locks the mutex so won't be very efficient for high
         * numbers of connections.  TODO: Make more efficient ?  Is
         * it worth while to do so?
         */
        void cleanup() noexcept
        {
            // What's our timeout?
            milliseconds timeout = duration_cast<milliseconds> (
                std::chrono::system_clock::now().time_since_epoch()
            );

            // subtract 5 seconds -- TODO: make this configurable
            timeout -= milliseconds(5000);

            this->mtx.lock();

            auto it = this->sessions.begin();
            while(it != this->sessions.end()) {
                if((!it->second) || (it->second->lastAccessTime < timeout)) {
                    delete it->second;
                    it->second = NULL;
                    it = this->sessions.erase(it);
                } else {
                    ++it;
                }
            }

            this->mtx.unlock();
        }
         

    private:
        // Resolved server info
        struct addrinfo*                                servinfo;

        // Our session map
        std::unordered_map<uint64_t, RTMPConnection*>   sessions;

        // Our mutex for the session map
        std::mutex                                      mtx;
};


/**************************************************************************
 *
 * GLOBAL VARIABLES
 *
 *************************************************************************/

// Our RTMP server connector!  Global for the connect method mostly
static RTMPServer* server;


/**************************************************************************
 *
 * HANDLERS
 *
 *************************************************************************/


/*
 * This is a generic error handler just to send a 500.  rtmp can be NULL;
 * if its not, we'll free it.
 */
static int send_error_response(struct mg_connection *conn,
                               RTMPConnection* rtmp)
{
    mg_printf(conn,
              "HTTP/1.1 500 SERVER ERROR\r\n"
              "Connection: Close\r\n"
              "Content-Length: 0\r\n"
              "Cache-Control: no-cache\r\n\r\n");

    if(rtmp) {
        delete rtmp;
    }

    return 500;
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
              "Connection: Keep-Alive\r\n"
              "Content-Length: 0\r\n"
              "Cache-Control: no-cache\r\n\r\n");

    return 200;
}

/*
 * The RTMPT open handler that starts a session.
 */
static int open_handler(struct mg_connection *conn, void *ignored)
{
    uint64_t        session_id;
    int             err;

    // Make our session
    try {
        session_id = server->connect();
    } catch(std::runtime_error& e) {
        // Disable warning that's just on this line
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-security"
        mg_cry(conn, e.what());
#pragma clang diagnostic pop
        return send_error_response(conn, NULL);
    }

    // Convert integer to string
    std::string id = std::to_string(session_id);

    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/x-fcs\r\n"
              "Connection: Keep-Alive\r\n"
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
    RTMPConnection*                     rtmp;
    char                                buf[16384];
    int                                 size;
    size_t                              total_read = 0;
    char*                               superBuf = NULL;
    size_t                              superSize = 0;

    rtmp = server->checkout(conn);

    if(!rtmp) {
        return send_error_response(conn, NULL);
    }

    while(rtmp->hasData()) {
        // read
        size = recv(rtmp->sock, buf, 16383, 0);

        if(size == 0) {
            // show's over, folks
            mg_cry(conn, "Remote connection closed");
            return send_error_response(conn, rtmp);
        } else if(size < 0) {
            // show's still over, folks.
            mg_cry(conn, "Remote connection error");
            return send_error_response(conn, rtmp);
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
        rtmp->resetEmpty();
    } else {
        rtmp->increaseEmpty();
    }

    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/x-fcs\r\n"
              "Connection: Keep-Alive\r\n"
              "Cache-Control: no-cache\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: %lu\r\n\r\n",
              superSize+1);

    mg_write(conn, &rtmp->delay, 1);
    mg_write(conn, superBuf, superSize);

    free(superBuf);
    server->checkin(rtmp); // return it.

    return 200;
}

/*
 * Handle the send request, which is how the client sends
 * data over the socket
 */
static int send_handler(struct mg_connection *conn, void *ignored)
{
    RTMPConnection*                     rtmp;
    char                                buf[8192];
    size_t                              size;

    rtmp = server->checkout(conn);

    if(!rtmp) {
        return send_error_response(conn, NULL);
    }

    // Get data and push to RTMP
    while((size = mg_read(conn, buf, 8192)) > 0) {
        // Send it
        int total = 0;
        int bytesleft = size;
        int n;

        // Send it all.
        while(total < size) {
            n = send(rtmp->sock, buf+total, bytesleft, 0);

            if(n == -1) {
                mg_cry(conn, "Lost connection while sending!");
                return send_error_response(conn, rtmp);
            }

            total += n;
            bytesleft -= n;
        }
    }

    // Silly to check back in just to check out again.
    // TODO: keep checked out?
    server->checkin(rtmp);

    // Route to idle handler after data is sent.  Handling is
    // the same from that point.
    return idle_handler(conn, ignored);
}

/*
 * Close the handler
 */
static int close_handler(struct mg_connection *conn, void *ignored)
{
    RTMPConnection* rtmp;

    rtmp = server->checkout(conn);

    if(!rtmp) {
        return send_error_response(conn, NULL);
    }

    delete rtmp;

    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/x-fcs\r\n"
              "Connection: Close\r\n"
              "Cache-Control: no-cache\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: 1\r\n\r\n"
    );

    mg_write(conn, "\0", 1);

    return 200;
}

/*
 * Main
 *
 * Arguments, in order:
 *
 * * RTMP Server Host (required)
 * * RTMP Server Port (required)
 *
 * And then anything civet supports, in key then value sequence
 *
 * We add a 'run_as_group' option that civet doesn't support.
 */
int main(int argc, char** argv, char** envp)
{
    // Settings
    const char**      options = NULL;

    // server context
    struct mg_context   *ctx;

    // Parse arguments
    if((argc < 3) || (!(argc % 2))) { // invalid
        std::cerr << "Syntax: " << argv[0]
                                << " RemoteServerHost"
                                << " RemoteServerPort"
                                << " [civet-option civet-value]"
                                << " [... as many as you want]"
                                << std::endl;
        return -1;
    }

    options = (const char**)malloc(sizeof(const char*) * (argc));
    int j = 0;

    // These are civet arguments.
    if(argc > 3) {

        // Parse 'em
        for(int i = 3; i < argc; i+=2) {
            if(!strcmp(argv[i], "run_as_group")) {
                // we handle this one
                struct group* myGroup = getgrnam(argv[i+1]);

                if(!myGroup) {
                    std::cerr << "Unknown group: " << argv[i+1]
                              << std::endl;
                    return -1;
                }

                if(setgid(myGroup->gr_gid) < 0) {
                    perror("Could not set group");
                    return -1;
                }
            } else if(!strcmp(argv[i], "enable_keep_alive")) {
                // ignore this parameter -- we will add it
                continue;
            } else {
                options[j] = argv[i];
                options[++j] = argv[i+1];
                j++;
            }
        }
    }

    // add keep alive
    options[j] = "enable_keep_alive";
    options[++j] = "yes";
    options[++j] = NULL;

    // init library - use IPv6
    mg_init_library(8);

    // start server
    ctx = mg_start(NULL, NULL, options);

    // Make sure it actually started
    if(!ctx) {
        return -1;
    }

    // try to connect to server.
    try {
        server = new RTMPServer(argv[1], argv[2]);
    } catch(std::runtime_error& e) {
        // Probably couldn't resolve host.
        std::cerr << e.what() << std::endl;
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

        // run cleanup
        server->cleanup();
    }

    // Code will never get here.  But in case we ever want a legit
    // shutdown call, this is what we'd do.

    // Stop server
    mg_stop(ctx);

    // Un-init
    mg_exit_library();

    free(options);

    return (int) 0;
}
