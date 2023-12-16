#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

int main() {
	int sockfd;  
	struct addrinfo hints, *servinfo, *p;
	int rv;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC; // use AF_INET6 to force IPv6
	hints.ai_socktype = SOCK_STREAM;

	if ((rv = getaddrinfo("www.yahoo.com", "http", &hints, &servinfo)) != 0) {
	    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
	    exit(1);
	}
	
	// loop through all the results and connect to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
	    if ((sockfd = socket(p->ai_family, p->ai_socktype,
	            p->ai_protocol)) == -1) {
	        perror("socket");
	        continue;
	    }
	
	    if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
	        perror("connect");
	        close(sockfd);
	        continue;
	    }
	
	    break; // if we get here, we must have connected successfully
	}
	
	if (p == NULL) {
	    // looped off the end of the list with no connection
	    fprintf(stderr, "failed to connect\n");
	    exit(2);
	}
	
	freeaddrinfo(servinfo); // all done with this structure
}
