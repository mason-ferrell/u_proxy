#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <semaphore.h>
#include <dirent.h>

#define BUFSIZE 4096

//function to ensure entire response is written to client
int socket_write(int sock, char *message, int stream_size) {
	int bytes_written, unsent_bytes, new_bytes_written;
	
	bytes_written = write(sock, message, stream_size);
	if(bytes_written < 0) return -1;
	unsent_bytes = stream_size - bytes_written;
	
	while(unsent_bytes > 0) {
		new_bytes_written = write(sock, message+bytes_written, unsent_bytes);
		if(new_bytes_written < 0) return -1;
		
		bytes_written += new_bytes_written;
		unsent_bytes = stream_size - bytes_written;
	}
	return 0;
}

//hash function for caching server responses
//from Dan Bernstein, http://www.cse.yorku.ca/~oz/hash.html
unsigned long fileHash(unsigned char *str) {
    unsigned long hash = 5381;
    int c;

    while (c = *str++)
        hash = ((hash << 5) + hash) + c;

    return hash;
}

//these three function declarations are for general functionality
void *proxy_func(void *);
int parse_get_request(char*,char**,char**,char**,char*);
void send_error_message(int, int, char*);

//function to reply to request when info is cached
void send_cached_response(int, FILE *);

//forward_and_cache forwards the client's request and caches the server's response
//parse_uri, connect_to_host, blocklisted, and cache_response are all helper functions
void forward_and_cache(char *, int, char *, char *);
int parse_uri(char *, char **, char **);
int connect_to_host(int *, char *);
int blocklisted(char *);
void cache_response(char *, int, int);

//these two functions are specific to working with the cache
FILE *find(unsigned long, char *, int);
void *clear_cache(void *);

//binary semaphores used to synchonize cache access. See synchonization section of submitted file "notes" for more information
sem_t wrt;
sem_t mutex;
sem_t search_mutex;

//global variables used for synchronization
int writers = 0;
int readers = 0;

typedef struct {
	int client_sock;
	int timeout;
} proxy_args;

int main(int argc, char *argv[]) {
	int sockfd, client_sock;
	struct sockaddr_in proxy, client;
	int clientlen;
	struct stat st = {0};
	int timeout;
	
	if(argc != 3) {
		printf("Usage %s <port #> <cache timeout in sec>\n", argv[0]);
		exit(-1);
	}
	
	//if cache folder does not exist, create one
	if (stat("./cache/", &st) == -1) {
    		mkdir("./cache/", 0777);
	}
	
	//set up pthread attributes
	pthread_attr_t attr;
    	pthread_attr_init(&attr);
    	
    	//initializes mutexes
    	sem_init(&wrt, 0, 1);
    	sem_init(&mutex, 0, 1);
    	sem_init(&search_mutex, 0, 1);
	
	//create/open proxy socket
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if(sockfd < 0)
		perror("opening socket");
		
	int optval = 1;
	if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *) &optval, sizeof(int)) < 0)
		perror("setting reuseaddr");
	
	//populate proxy info
	proxy.sin_family = AF_INET;
	proxy.sin_addr.s_addr = INADDR_ANY;
	proxy.sin_port = htons(atoi(argv[1]));
	
	//bind proxy, set listen queue to 3
	if(bind(sockfd, (struct sockaddr *)&proxy, sizeof(proxy)) < 0)
		perror("binding socket");

	listen(sockfd, 3);
	
	clientlen = sizeof(client);
	
	timeout = atoi(argv[2]);
	if(timeout < 0) timeout = 0;
	
	//run thread to periodically check cache files and clear any unnecessary ones
	pthread_t d;
	pthread_create(&d, &attr, clear_cache, (void *)&timeout);
	
	while(1) {
		proxy_args pa;
		proxy_args *arg_ptr;
		
		client_sock = accept(sockfd, (struct sockaddr *)&client,(socklen_t *)&clientlen);
		if(client_sock < 0) {
			perror("accepting connection");
			continue;
		}
		
		pa.client_sock = client_sock;
		pa.timeout = timeout;
		
		pthread_t runner;
	
		arg_ptr = malloc(sizeof(proxy_args));
		if(arg_ptr==NULL) perror("malloc before proxy thread creation");
		*arg_ptr = pa;		
		
		pthread_create(&runner, &attr, proxy_func, (void *) arg_ptr);
		
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//this is the main function for each thread except cache deleter thread
void *proxy_func(void *pa) {
	int server_sock;
	int client_sock = ((proxy_args *) pa)->client_sock;
	int timeout = ((proxy_args *)pa)->timeout;
	free(pa);
	
	char buffer[BUFSIZE], proxy_req[BUFSIZE];
	char *command, *uri, *version;
	int err;
	unsigned long int hash;
	
	bzero(buffer, BUFSIZE);
	bzero(proxy_req, BUFSIZE);
	command = uri = version = NULL;
	
	//sometimes an empty message is received, ignore these and erroneous calls
	if(recv(client_sock, buffer, BUFSIZE, 0) <= 0) {
		if(close(client_sock) < 0) perror("closing socket");
		return NULL;
	}
		
	//parse_get_request returns any relevant error codes
	err = parse_get_request(buffer, &command, &uri, &version, proxy_req);
		
	if(err!=0) {
		send_error_message(client_sock, err, version);
		if(close(client_sock) < 0) perror("closing socket");
		return NULL;
	}
	
	FILE *cache_file;
	hash = fileHash(uri);
	
	//see notes on synchonization for this part
	sem_wait(&search_mutex);
	cache_file = find(hash, uri, timeout);
	
	if(cache_file) {
		sem_post(&search_mutex);
		send_cached_response(client_sock, cache_file);
		printf("Got file contents from cache\n");
	}
	else {
		forward_and_cache(version, client_sock, proxy_req, uri);
		sem_post(&search_mutex);
		printf("Got file contents from network\n");
	}
}

//this function parses get requests and puts each individual chunk into a string passed into the function by reference
//if there is an error in the request, return the appropriate error number
int parse_get_request(char *buffer, char **command, char **uri, char **version, char *proxy_req){

	//make sure no overflows are attempted
	if(buffer[BUFSIZE - 1] != 0) return 400;
	strcpy(proxy_req, buffer);
	
	*command = strtok(buffer, " \t\n\r");
	if(*command==NULL) return 400;
	if(strcmp(*command, "HEAD")==0 || strcmp(*command, "POST")==0 || strcmp(*command, "PUT")==0) return 405;
	if(strcmp(*command, "GET")!=0) return 400;
	
	*uri = strtok(NULL, " \t\n\r");
	if(*uri==NULL) return 400;
	
	*version = strtok(NULL, " \t\n\r");
	if(*version==NULL) return 400;
	if(strcmp(*version, "HTTP/1.0")!=0 && strcmp(*version, "HTTP/1.1")!=0) {
		*version = NULL;
		return 505;
	}
	
	return 0;
}	

//formats and sends an error message based on error number
void send_error_message(int client_sock, int err, char *version) {
	char message[256];
	int stream_size;
	if(version == NULL) strcpy(message, "HTTP/1.1");
	else strcpy(message, version);

	if(err==400) strcat(message, " 400 Bad Request\r\n");
	else if(err==403) strcat(message, " 403 Forbidden\r\n");
	else if(err==404) strcat(message, " 404 Not Found\r\n");
	else if(err==405) strcat(message, " 405 Method Not Allowed\r\n");
	else if(err==505) strcat(message, " 505 HTTP Version Not Supported\r\n");
	else perror("programmer messed up error codes, :(");
	
	strcat(message, "\r\n");
	
	stream_size = strlen(message);
	if(socket_write(client_sock, message, stream_size) < 0) perror("writing to socket, line 254ish");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//this function simply takes the cached file, cuts off the first line (which has the URI as a form of error detection),
//and sends the remainder of the file, which is the cached response
void send_cached_response(int sock, FILE *fp) {
	int start_offset;
	int response_size;
	int bytes_read, unread_bytes, new_bytes_read;
	
	start_offset = ftell(fp);
	fseek(fp, 0, SEEK_END);
	response_size = ftell(fp) - start_offset;
	fseek(fp, start_offset, SEEK_SET);
	
	//make message buffer big enough to contain entire response
	char message[response_size];
	
	bytes_read = fread(message, 1, response_size, fp);
	if(bytes_read < 0) perror("reading file");
	
	//make sure entire file is read
	unread_bytes = response_size - bytes_read;
	while(unread_bytes > 0) {
		new_bytes_read = fread(message, 1, unread_bytes, fp);
		if(new_bytes_read < 0) perror("reading cached file");
		
		bytes_read += new_bytes_read;
		unread_bytes = response_size - bytes_read;
	}
	
	if(socket_write(sock, message, response_size)<0)
		perror("writing to socket around line 287");
	if(fclose(fp)!=0) perror("closing file");
	if(close(sock) < 0) perror("closing socket");
	
	//see synchronization
	sem_wait(&mutex);
	readers--;
	if(readers==0) sem_post(&wrt);
	sem_post(&mutex);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//if requested file is not found in cache, forward request to server and response back to client
//then cache server's response
void forward_and_cache(char *version, int client_sock, char *proxy_req, char *uri) {
	char *hostname, *file;
	char proxy_forward[BUFSIZE], buffer[BUFSIZE];
	int server_sock;
	char uri_copy[strlen(uri)+1];
	char *header_line;
	int err;
	
	strcpy(uri_copy, uri);
	
	//set up connection to server
	err = parse_uri(uri_copy, &hostname, &file);
	if(err!=0) {
		send_error_message(client_sock, err, version);
		if(close(client_sock) < 0) perror("closing socket");
		return;
	}
	
	err = connect_to_host(&server_sock, hostname);
	if(err!=0) {
		send_error_message(client_sock, err, version);
		if(close(client_sock) < 0) perror("closing socket");
		return;
	}
	
	//next two chunks of code formulate http request from proxy to server
	bzero(proxy_forward, BUFSIZE);
	strcpy(proxy_forward, "GET /");
	if(file!=NULL) strcat(proxy_forward, file);
	strcat(proxy_forward, " ");
	if(version!=NULL) strcat(proxy_forward, version);
	else strcat(proxy_forward, "HTTP/1.1");
	strcat(proxy_forward, "\r\n");
	
	header_line = strtok(proxy_req, "\r\n"); //scan past first header line
	
	//copy over headers, ignoring anything related to persistent connections
	while((header_line = strtok(NULL, "\r\n")) != NULL) {
		if(strcasecmp(header_line, "Proxy-Connection: keep-alive")!=0 && strcasecmp(header_line, "Connection: keep-alive")!=0) {
			strcat(proxy_forward, header_line);
			strcat(proxy_forward, "\r\n");
		}
		if(strcasecmp(header_line, "Connection: keep-alive")==0)
			strcat(proxy_forward, "Connection: close\r\n");
	}
	strcat(proxy_forward, "\r\n");
	
	if(socket_write(server_sock, proxy_forward, strlen(proxy_forward)) < 0) perror("writing to socket line 349ish");
	
	cache_response(uri, client_sock, server_sock);
	
	if(close(server_sock) < 0) perror("closing socket");
	if(close(client_sock) < 0) perror("closing socket");
}

//parses uri, checks uri is of valid format
int parse_uri(char *uri, char **hostname, char **file) {
	char *protocol;
	
	protocol = strtok(uri, ":/");
	if(protocol==NULL || strcmp(protocol, "http")!= 0)
		return 400;
	
	*hostname = strtok(NULL, "/");
	if(*hostname==NULL)
		return 404;
	
	*file = strtok(NULL, " \r\n");
	return 0;
}

//determines host IP and port for server and sets up connection
int connect_to_host(int *server_sock, char *hostname) {
	char stackbuffer = '\0';

	char host_port[strlen(hostname)+1];
	struct addrinfo hints, *servinfo, *p;
	char *host, *port, port_str[8];
	
	bzero(port_str, 8);
	strcpy(host_port, hostname);
	host = strtok(host_port, ":");
	port = strtok(NULL, ":");
	if(port==NULL) strcpy(port_str, "http");
	else strcpy(port_str, port);
	
	if(blocklisted(host)) return 403;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	
	if (getaddrinfo(host, port_str, &hints, &servinfo) != 0) {
	    return 404;
	}
	
	/*p = servinfo;
	struct sockaddr_in *h = (struct sockaddr_in *)p->ai_addr;
	char *ip = inet_ntoa(h->sin_addr);
	printf("ip is %s\n", ip);
	int n = 0;*/
	
	for(p = servinfo; p != NULL; p = p->ai_next) {
   		if ((*server_sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
		        perror("socket");
        		continue;
    		}

        	if (connect(*server_sock, p->ai_addr, p->ai_addrlen) == -1) {
        		perror("connect");
        		if(close(*server_sock) < 0) perror("closing socket");
        		continue;
    		}

    		break;
	}
	
	return 0;
}

//checks hostnames against blocklist
int blocklisted(char *host) {
	FILE *bl = fopen("./blocklist", "r");
	if(bl==NULL) return 0;
	
	char buffer[128];
	
	bzero(buffer, 128);
	while(fgets(buffer, 50, bl)!=NULL) {
		if(buffer[127]!='\0')
			perror("OVERFLOW IN BLOCKLISTED FUNCTION");
	
		buffer[strlen(buffer) - 1] = '\0';
		if(strcasecmp(host, buffer)==0) {
			fclose(bl);
			return 403;
		}
	}
	fclose(bl);
	return 0;
}

//this function caches the response from the server and forwards it to client
void cache_response(char *uri_copy, int client_sock, int server_sock) {
	//check that file is not dynamic content
	char *uri = strtok(uri_copy, "?");
	char *dynamic = strtok(NULL, "?");
	
	int byte_transfer;
	unsigned long int hash = fileHash(uri_copy);
	char hash_str[100];
	strcpy(hash_str, "./cache/");
	sprintf(hash_str + 8, "%lu", hash);
	char buffer[BUFSIZE];
	FILE *fp;
	
	//see synchronization notes
	sem_wait(&mutex);
	writers++;
	sem_post(&mutex);
	
	if(!dynamic) {
		bzero(buffer, BUFSIZE);
		strcpy(buffer, uri_copy);
		strcat(buffer, "\n");
		fp = fopen(hash_str, "w");
		fwrite(buffer, 1, strlen(buffer), fp);
	}
	
	bzero(buffer, BUFSIZE);
	while((byte_transfer = recv(server_sock, buffer, BUFSIZE, 0)) > 0) {
		//write response from server to both client socket and cache file
		if(socket_write(client_sock, buffer, byte_transfer) < 0) perror("writing to socket, line 476ish");
		if(!dynamic) fwrite(buffer, 1, byte_transfer, fp);
		bzero(buffer, BUFSIZE);
	}
	
	if(!dynamic) fclose(fp);
	
	sem_wait(&mutex);
	writers--;
	sem_post(&mutex);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//this function finds the cached file if it exists
FILE *find(unsigned long int hash, char *uri, int timeout) {
	if(timeout==0) return NULL;

	char hash_str[128], first_line[BUFSIZE];
	FILE *fp;
	struct stat file_info;
	time_t cur_time;
	
	strcpy(hash_str, "./cache/");
	sprintf(hash_str + 8, "%lu", hash);
	
	sem_wait(&mutex);
	if(writers > 0) {
		sem_post(&mutex);
		sem_wait(&wrt);
		sem_wait(&mutex);
	} else if (readers==0) {
		sem_post(&mutex);
		sem_wait(&wrt);
		sem_wait(&mutex);
	}
	readers++;
	sem_post(&mutex);
	
	fp = fopen(hash_str, "r");
	if(fp==NULL) {
		sem_wait(&mutex);
		readers--;
		if(readers==0) sem_post(&wrt);
		sem_post(&mutex);
		return NULL;
	}
	
	fgets(first_line, BUFSIZE, fp);
	first_line[strlen(first_line) - 1] = '\0';
		
	stat(hash_str, &file_info);
		
	time(&cur_time);
		
	if(strcmp(uri, first_line)==0 && difftime(cur_time, file_info.st_mtime) <= timeout)
		return fp;
	if(fp!=NULL) fclose(fp);
	
	sem_wait(&mutex);
	readers--;
	if(readers==0) sem_post(&wrt);
	sem_post(&mutex);
	
	return NULL;
}

//this function periodically scans the entire cache directory and removes any files
//with timestamps that have expired past the given cache expiration time
void *clear_cache (void *timeout_ptr) {
	int timeout = *(int *) timeout_ptr;
	struct dirent *d;
	DIR *dh = opendir("./cache");
	if(!dh) perror("opening directory");
	struct stat file_info;
	time_t cur_time;
	char filepath[128];
	strcpy(filepath, "./cache/");
	
	while(1) {
		sem_wait(&mutex);
		writers++;
		sem_post(&mutex);
		sem_wait(&wrt);

		rewinddir(dh);
	
		while((d = readdir(dh)) != NULL) {
			if(d->d_name[0] == '.') continue;
			strcpy(filepath + 8, d->d_name);
			stat(filepath, &file_info);
			
			time(&cur_time);
			
			if(difftime(cur_time, file_info.st_mtime) > timeout) {
				remove(filepath);
			}
		}
		
		sem_post(&wrt);  //if deadlock, try switching these and see what happens
		sem_wait(&mutex);
		writers--;
		sem_post(&mutex);
		
		if(timeout==0) break;
		
		sleep(timeout);
	}
}
