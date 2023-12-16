#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>

int main() {
	char msg[100];
	strcpy(msg,"http://localhost:8080/index.html");
	
	char *protocol;
	char *hostname;
	char *file;
	
	protocol = strtok(msg, ":/");
	if(strcmp(protocol, "http")!= 0) {
		printf("Must use http\n");
		return -1;
	}
	
	hostname = strtok(NULL, "/");
	if(hostname==NULL) {
		printf("Must have hostname\n");
	}
	
	file = strtok(NULL, " \r\n");
	if(file==NULL) {
		printf("File must be referenced\n");
		return -1;
	}
	
	printf("Hostname is %s\nFile is %s\n", hostname, file);
	
	struct addrinfo hints, *servinfo;
	int rv;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	
	if ((rv = getaddrinfo(hostname, "http", &hints, &servinfo)) != 0) {
	    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
	    exit(1);
	}
	
	struct sockaddr_in *host;
	
	host = (struct sockaddr_in *)servinfo->ai_addr;
	printf("host is %s\n", inet_ntoa(host->sin_addr));
	
	return 0;
}
