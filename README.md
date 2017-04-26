# rtmptd
C++ RTMPT Proxy Implementation

# WHY?

RTMPT is a tunnel protocol that allows you to tunnel RTMP (Adobe Flash streams) through an HTTP request.  This is great for users behind proxies who might not otherwise be able to view your streams.

All commercial RTMP offerings have a built in RTMPT proxy.  However, the free servers are sorely lacking.  NGINX has an RTMPT proxy module, but its horridly written and seg-faults constantly.  I was trying to fix that module, when I concluded it would be way easier to write my own from scratch.

This one uses C++ and CivetWeb server.  I cribbed heavily off this Java server that I found, but the Java server didn't have the necessary performance for what I needed so I re-wrote it in C++.  However, I used that code as a foundation for my own.

I wrote this in basically 1 day so please be easy on me!

# LINKS

This is the RTMPT Java server I cribbed from : https://github.com/lindenbaum/rtmpt-proxy/

Thanks lindenbaum!!

This is Civetweb, you will need it to compile this:

https://github.com/civetweb/civetweb/

# REQUIREMENTS

Civetweb as noted.  Thread Building Blocks and Boost Random are also required.

# BUILDING
cmake .
make

I put about 2 seconds into the build process here so some assembly
will be required :)  I'm not planning on Windows support, but it
should compile cleanly on Windows if you want to go on that
adventure yourself.

