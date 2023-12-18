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

void error(char *msg) {
	printf("Error %s\n", msg);
	exit(-1);
}

//function to ensure entire response is written to client
void socket_write(int sock, char *message, int stream_size) {
	int bytes_written, unsent_bytes, new_bytes_written;
	
	bytes_written = write(sock, message, stream_size);
	if(bytes_written < 0) error("writing to socket");
	unsent_bytes = stream_size - bytes_written;
	
	while(unsent_bytes > 0) {
		new_bytes_written = write(sock, message+bytes_written, unsent_bytes);
		if(new_bytes_written < 0) error("writing to socket");
		
		bytes_written += new_bytes_written;
		unsent_bytes = stream_size - bytes_written;
	}
}

//hash function for caching server responses
unsigned long fileHash(unsigned char *str) {
    unsigned long hash = 5381;
    int c;

    while (c = *str++)
        hash = ((hash << 5) + hash) + c;

    return hash;
}

int parse_get_request(char*, char**, char**, char**, char**);
void send_error_message(int, int, char *);
void *proxy_func(void *);
void send_cached_response(int, FILE *);
void forward_and_cache(char *, int, char *, char *);
FILE *find(unsigned long, char *);
void cache_response(char *, int, int);
void *clear_cache(void *);

sem_t wrt;
sem_t mutex;

int writers = 0;
int readers = 0;

int main(int argc, char *argv[]) {
	int sockfd, client_sock;
	int *sock_ptr;
	struct sockaddr_in proxy, client;
	int clientlen;
	
	struct stat st = {0};

	if (stat("./cache/", &st) == -1) {
    		mkdir("./cache/", 0777);
	}
	
	if(argc != 2) {
		printf("Usage %s <port #>\n", argv[0]);
		exit(-1);
	}
	
	//set up pthread attributes
	pthread_attr_t attr;
    	pthread_attr_init(&attr);
    	
    	sem_init(&wrt, 0, 1);
    	sem_init(&mutex, 0, 1);
	
	//create/open proxy socket
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if(sockfd < 0)
		error("opening socket");
		
	int optval = 1;
	if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *) &optval, sizeof(int)) < 0)
		error("setting reuseaddr");
	
	//populate proxy info
	proxy.sin_family = AF_INET;
	proxy.sin_addr.s_addr = INADDR_ANY;
	proxy.sin_port = htons(atoi(argv[1]));
	
	//bind proxy, set listen queue to 3
	if(bind(sockfd, (struct sockaddr *)&proxy, sizeof(proxy)) < 0)
		error("binding socket");

	listen(sockfd, 3);
	
	clientlen = sizeof(client);
	
	pthread_t d;
	pthread_create(&d, &attr, clear_cache, NULL);
	
	while(1) {
		client_sock = accept(sockfd, (struct sockaddr *)&client,(socklen_t *)&clientlen);
		if(client_sock < 0)
			error("accepting connection");
		
		pthread_t runner;
		
		//use sock_ptr to avoid race condition when starting thread, don't forget to free at end of http function
		sock_ptr = malloc(sizeof(int));
		*sock_ptr = client_sock;		
		
		pthread_create(&runner, &attr, proxy_func, (void *) sock_ptr);
		
	}
}

//this function parses get requests and puts each individual chunk into a string passed into the function by reference
//if there is an error in the request, return the appropriate error number
int parse_get_request(char *buffer, char **command, char **uri, char **version, char **ext){
	char *dot;
	
	if(buffer[BUFSIZE - 1] != 0) return 400;
	
	*command = strtok(buffer, " \t\n\r");
	if(*command==NULL) return 400;
	if(strcmp(*command, "HEAD")==0 || strcmp(*command, "POST")==0) return 405;
	if(strcmp(*command, "GET")!=0) return 400;
	
	*uri = strtok(NULL, " \t\n\r");
	if(*uri==NULL) return 400;
	
	*version = strtok(NULL, " \t\n\r");
	if(*version==NULL) return 400;
	if(strcmp(*version, "HTTP/1.0")!=0 && strcmp(*version, "HTTP/1.1")!=0) return 505;
	
	dot = strrchr(*uri, '.');
	if(dot==NULL) *ext = NULL;
	else *ext = dot + 1;
	
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
	else error("programmer messed up error codes, :(");
	
	strcat(message, "\r\n");
	
	stream_size = strlen(message);
	socket_write(client_sock, message, stream_size);
}

void *proxy_func(void *cs) {
	int server_sock;
	int client_sock = *(int *)cs;
	free(cs);
	
	char buffer[BUFSIZE], proxy_req[BUFSIZE], proxy_forward[BUFSIZE]; //error check overflows to this
	char *command, *uri, *version, *ext;
	int err;
	unsigned long int hash;
	
	bzero(buffer, BUFSIZE);
	bzero(proxy_req, BUFSIZE);
	command = uri = version = ext = NULL;
	
	//sometimes an empty message is received, ignore these and erroneous calls
	if(recv(client_sock, buffer, BUFSIZE, 0) <= 0) {
		if(close(client_sock) < 0) error("closing socket");
		return NULL;
	}
		
	//parse_get_request returns any relevant error codes
	strcpy(proxy_req, buffer);
	err = parse_get_request(buffer, &command, &uri, &version, &ext); //DON'T NEED ext
		
	if(err!=0) {
		send_error_message(client_sock, err, version);
		if(close(client_sock) < 0) error("closing socket");
		return NULL;
	}
	
	FILE *cache_file;
	hash = fileHash(uri);
	
	cache_file = find(hash, uri);
	
	if(cache_file) {
		send_cached_response(client_sock, cache_file);
		printf("Got file contents from cache\n");
	}
	else {
		forward_and_cache(uri, client_sock, proxy_req, version);
		sem_wait(&mutex);
		readers--;
		if(readers==0) sem_post(&wrt);
		sem_post(&mutex);
		printf("Got file contents from network\n");
	}
}

int parse_uri(char *uri, char **host, char **file) {
	char *protocol;
	
	protocol = strtok(uri, ":/");
	if(protocol==NULL || strcmp(protocol, "http")!= 0) {
		printf("Must use http\n"); //send error request
		return 400;
	}
	
	*host = strtok(NULL, "/");
	if(*host==NULL) {
		printf("Need hostname\n");
		return 404;
	}
	
	*file = strtok(NULL, " \r\n");
}

int connect_to_host(char *hostname, int *server_sock) {
	char host_port[50];
	struct addrinfo hints, *servinfo, *p;
	char *host, *port, port_str[10];
	
	bzero(port_str, 10);	
	strcpy(host_port, hostname);
	host = strtok(host_port, ":");
	port = strtok(NULL, ":");
	if(port==NULL) strcpy(port_str, "http");
	else strcpy(port_str, port);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	
	if (getaddrinfo(host, port_str, &hints, &servinfo) != 0) {
	    return 404;
	}
	
	for(p = servinfo; p != NULL; p = p->ai_next) {
   		if ((*server_sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
		        perror("socket");
        		continue;
    		}

        	if (connect(*server_sock, p->ai_addr, p->ai_addrlen) == -1) {
        		perror("connect");
        		close(*server_sock);
        		continue;
    		}

    		break; // if we get here, we must have connected successfully
	}
	
	/*struct sockaddr_in *host_addr;
	
	host_addr = (struct sockaddr_in *)servinfo->ai_addr;
	printf("host is %s\n", inet_ntoa(host_addr->sin_addr));*/
	
	return 0;
}

void forward_and_cache(char *uri, int client_sock, char *proxy_req, char *version) {
	char *hostname, *file;
	char proxy_forward[BUFSIZE], buffer[BUFSIZE];
	int server_sock;
	char uri_copy[100];
	char *header_line;
	int err;
	
	strcpy(uri_copy, uri);
	
	err = parse_uri(uri, &hostname, &file);
	
	err = connect_to_host(hostname, &server_sock);
	
	//next two chunks of code formulate http request from proxy to server
	bzero(proxy_forward, BUFSIZE);
	strcpy(proxy_forward, "GET /");
	if(file!=NULL) strcat(proxy_forward, file);
	strcat(proxy_forward, " ");
	strcat(proxy_forward, version);
	strcat(proxy_forward, "\r\n");
	
	//error handle later
	header_line = strtok(proxy_req, "\r\n");
	while((header_line = strtok(NULL, "\r\n")) != NULL) {
		if(strcasecmp(header_line, "Proxy-Connection: keep-alive")!=0 && strcasecmp(header_line, "Connection: keep-alive")!=0) {
			strcat(proxy_forward, header_line);
			strcat(proxy_forward, "\r\n");
		}
		if(strcasecmp(header_line, "Connection: keep-alive")==0)
			strcat(proxy_forward, "Connection: close\r\n");
	}
	strcat(proxy_forward, "\r\n");
	
	socket_write(server_sock, proxy_forward, strlen(proxy_forward));
	
	cache_response(uri_copy, client_sock, server_sock);
}

void cache_response(char *uri_copy, int client_sock, int server_sock) {
	int byte_transfer;
	unsigned long int hash = fileHash(uri_copy);
	char hash_str[100];
	strcpy(hash_str, "./cache/");
	sprintf(hash_str + 8, "%lu", hash);
	char buffer[BUFSIZE];
	
	sem_wait(&mutex);
	writers++;
	sem_post(&mutex);
	
	FILE *fp = fopen(hash_str, "w");
	
	bzero(buffer, BUFSIZE);
	strcpy(buffer, uri_copy);
	strcat(buffer, "\n");
	fwrite(buffer, 1, strlen(buffer), fp);
	
	bzero(buffer, BUFSIZE);
	while((byte_transfer = recv(server_sock, buffer, BUFSIZE, 0)) > 0) {
		socket_write(client_sock, buffer, byte_transfer);
		fwrite(buffer, 1, byte_transfer, fp);
		bzero(buffer, BUFSIZE);
	}
	
	fclose(fp);
	
	sem_wait(&mutex);
	writers--;
	sem_post(&mutex);
	
	if(close(server_sock) < 0) error("closing socket");
	if(close(client_sock) < 0) error("closing socket");
}

void send_cached_response(int sock, FILE *fp) {
	int start_offset;
	int response_size;
	int bytes_read, unread_bytes, new_bytes_read;
	
	start_offset = ftell(fp);
	fseek(fp, 0, SEEK_END);
	response_size = ftell(fp) - start_offset;
	fseek(fp, start_offset, SEEK_SET);
	
	char message[response_size];
	
	bytes_read = fread(message, 1, response_size, fp);
	if(bytes_read < 0) error("reading file");
	
	//make sure entire file is read
	unread_bytes = response_size - bytes_read;
	while(unread_bytes > 0) {
		new_bytes_read = fread(message, 1, unread_bytes, fp);
		if(new_bytes_read < 0) error("writing to socket");
		
		bytes_read += new_bytes_read;
		unread_bytes = response_size - bytes_read;
	}
	
	socket_write(sock, message, response_size);
	fclose(fp);
	
	sem_wait(&mutex);
	readers--;
	if(readers==0) sem_post(&wrt);
	sem_post(&mutex);
}

FILE *find(unsigned long int hash, char *uri) {
	char hash_str[100], first_line[BUFSIZE];
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
	
	while((fp = fopen(hash_str, "r"))!=NULL) {
		fgets(first_line, BUFSIZE, fp);
		first_line[strlen(first_line) - 1] = '\0';
		
		stat(hash_str, &file_info);
		
		time(&cur_time);
		
		if(strcmp(uri, first_line)!=0 || difftime(cur_time, file_info.st_mtime) > 60) {
			hash++;
			sprintf(hash_str + 8, "%lu", hash);
		}
		else {
			return fp;
		}
	}
	
	return NULL;
}

void *clear_cache (void *arg) {
	struct dirent *d;
	DIR *dh = opendir("./cache");
	if(!dh) error("opening directory");
	struct stat file_info;
	time_t cur_time;
	char filepath[50];
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
			
			if(difftime(cur_time, file_info.st_mtime) > 60) {
				remove(filepath);
			}
		}
		
		sem_post(&wrt);  //if deadlock, try switching these and see what happens
		sem_wait(&mutex);
		writers--;
		sem_post(&mutex);
	}
}
