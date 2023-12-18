A basic HTTP proxy server. Implements a synchronized cache with a timeout specified by the user. Due to assignment requirements, this cache prioritizes minimizing network calls which sometimes can slow performance if a large file is requested while in the process of being cached.

To use the proxy, run the 'uproxy/proxy' binary or build using gcc and source file 'uproxy/uproxy.c'. Along with running the binary, two arguments are expected - the first specifies the port number the proxy will use, and the second specifices the TTL of cache items in seconds. Test using curl --proxy, or nc to the proxy and request using 'GET http://full-uri/path/to/requested/file HTTP/1'