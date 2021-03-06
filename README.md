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

Civetweb as noted.  Boost Random is also required.

# BUILDING
```
cmake .
make
```

Right now, it assumes civetweb-1.9.1 is in a sibling directory to your checkout of this.  My build process needs to be more configurable, but its unlikely I will bother in the near future unless there's some demand for it.

I'm not planning on Windows support, but it should compile cleanly on Windows if you want to go on that adventure yourself.

# RUNNING
It will build 'rtmptd' in the 'src' directory.  Copy it where you want.  It requires two arguments; a host and a password of the RTMP server you are proxying.  There are, additionally, optional arguments:

```
rtmptd RtmpServer RtmpPort [civet option] [civet option value] ...
```

for instance:

```
rtmptd localhost 1935 listening_ports 8080 num_threads 30
```

* RtmpServer and RtmpPort are are noted above -- RTMP is usually on port 1935
* CivetWeb, unfortunately, is a thread hog.  Each connection 'owns' a thread, and CivetWeb launches all num_threads right off the bat.  Instead of spinning threads as needed, if you make this, say, "100" it will go ahead and kick off 102 threads (Civet uses a couple of extra threads for itself).

I'd love to see Civet use either a smart polling mechanism or a more dynamic thread pool; I wouldn't be surprised if that was on their request queue.  But, for now, this is what we've got!

* Your options can be whatever Civet supports.  See:

https://github.com/civetweb/civetweb/blob/master/docs/UserManual.md

There's a section on options that explains it. 
* Civet is missing a "run_as_group" option, so I added it.  You can "run_as_group whateverName" to run as a certain group name in addition to Civet's "run_as_user" for a specific user name.


