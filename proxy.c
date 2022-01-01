#include <stdio.h>
#include <string.h>
#include "csapp.h"

/* 
 * Script for a proxy that handles HTTP/1.0 GET requests
 * and uses an LRU cache and threading server.
 * 
 * Grace Hunter
 * Wheaton College
 * CSCI 351 - Intro to Computer Systems
 */

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* cache line struct and cache */
struct line {
  int valid;
  int age;
  char* tag;
  char* buff;
};

struct line **cache;

static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:56.0) Gecko/20100101 Firefox/56.0\r\n";

/* semaphores for reading and writing */
int readers;
sem_t mutex, w;

struct line **construct_cache();
char* search_cache(char* request);
void store(char* request, char* buff);
void destroy_cache();

/* thread routinue - handle one client and close connection */
void *handle_client(void *arg){
  int *pfd = arg;
  int fd = *pfd;
  
  char buff[MAXLINE];  
  
  //var for parsing request
  char method[MAXLINE];
  char url[MAXLINE];
  char ver[MAXLINE];
  
  //vars for parsing headers
  char key[MAXLINE];
  char value[MAXLINE]; 
  
  size_t n;
  rio_t rio;
  
  Rio_readinitb(&rio, fd);
  
  //Read request
  n = Rio_readlineb(&rio, buff, MAXLINE);
  int s = sscanf(buff, "%s http://%s HTTP/%s", method, url, ver);
  if(s < 0){
    Rio_writen(fd, "HTTP/1.0 400 Bad request\n", 25);
    if(close(fd))
      perror("close fd");
    return NULL;
  }
  
  //extract hostname and uri
  char uri[MAXLINE];
  strncpy(uri, strchr(url, '/'), MAXLINE);
  char hostname[MAXLINE];
  strncpy(hostname, url, strchr(url, '/') - url);
  
  //extract port number
  char port[MAXLINE];
  char *port_location = strchr(hostname, ':');
  if(port_location != NULL){
    strncpy(port, port_location + 1, MAXLINE);
    hostname[port_location - hostname] = '\0';
  }
  else{
    port[0] = '\0';
  }
  
  //default port
  if(strlen(port) == 0){
    strncpy(port, "80\0", 3);;
  }
  
  //validate request
  if(strncmp(method, "GET", 3)){
    Rio_writen(fd, "HTTP/1.0 501 Not implemented", 29);
    if(close(fd))
      perror("close fd");
    return NULL;
  }
  
  //form request
  char request[MAXLINE] = "GET ";
  strncat(request, uri, MAXLINE);
  strncat(request, " HTTP/1.0\r\n", MAXLINE);
  
  //check cache
  P(&mutex);
  readers++;
  if(readers == 1)
    P(&w);
  V(&mutex);
  char* cache_response = search_cache(request);
  if(cache_response != NULL){
    Rio_writen(fd, cache_response, strlen(cache_response));
    if(close(fd) < 0)
      perror("close");
    P(&mutex);
    readers--;
    if(readers == 0)
      V(&w);
    V(&mutex);
    return NULL;
  }
  P(&mutex);
  readers--;
  if(readers == 0)
    V(&w);
  V(&mutex);
  
  /* open connection */
  int clientfd = open_clientfd(hostname, port);
  if(clientfd < 0)
    perror("open_clientfd");
  
  //write request
  Rio_writen(clientfd, request, strlen(request));
  
  //write host header
  char hostheader[MAXLINE] = "Host: ";
  strncat(hostheader, hostname, strlen(hostname));
  strncat(hostheader, "\r\n", 2);
  Rio_writen(clientfd, hostheader, strlen(hostheader));
  
  //write connection header
  char cheader[MAXLINE] = "Connection: close";
  strncat(cheader, "\r\n", 2);
  Rio_writen(clientfd, cheader, strlen(cheader));
  
  //write proxy-connection header
  char pcheader[MAXLINE] = "Proxy-Connection: close";
  strncat(pcheader, "\r\n", 2);
  Rio_writen(clientfd, pcheader, strlen(pcheader));
  
  //write user-agent header
  char uaheader[MAXLINE] = "User-Agent: ";
  strncat(uaheader, user_agent_hdr, strlen(user_agent_hdr));
  strncat(uaheader, "\r\n", 2);
  Rio_writen(clientfd, uaheader, strlen(uaheader));
  
  //read & forward request headers
  while((n = Rio_readlineb(&rio, buff, MAXLINE)) > 2){
    
    s = sscanf(buff, "%s %s", key, value);
    if(s < 0)
      fprintf(stderr, "Request header format error\n");
    
    if(strncmp("Host:", key, MAXLINE) 
          && strncmp("User-Agent:", key, MAXLINE) 
          && strncmp("Proxy-Connection:", key, MAXLINE) 
          && strncmp("Connection:", key, MAXLINE)){
      Rio_writen(clientfd, buff, n);
    }
  }
  
  Rio_writen(clientfd, "\r\n", 2); //end headers
  
  //receive and forward response
  char response[MAX_OBJECT_SIZE] = "";
  int len = 0;
  while((n = Rio_readn(clientfd, buff, MAXLINE)) > 2){
    Rio_writen(fd, buff, n);
    len += n;
    if(len <= MAX_OBJECT_SIZE){
      strncat(response, buff, n);
    }
  }
  
  //store response in cache
  if(len <= MAX_OBJECT_SIZE){
    P(&w); 
    store(request, response);
    V(&w);
  }
  
  if(close(clientfd) < 0)
    perror("close");
  if(close(fd) < 0)
    perror("close");
  return NULL;
  }
  
int main(int argc, char **argv){
    
  //construct cache and initialize semaphore
  cache  = construct_cache();
  readers = 0;
  Sem_init(&mutex, 0, 1);
  Sem_init(&w, 0, 1);;
  
  //open socket to listen
  char *port = argv[1];
  int lfd = open_listenfd(port);
  if(lfd < 0)
    perror("open_listenfd");
  
  //handle each client in a detached thread
  for(;;){
    int fd = accept(lfd, NULL, NULL);
    if(fd < 0)
      perror("accept");
    
    pthread_t thr;
    if(pthread_create(&thr, NULL, handle_client, &fd))
      fprintf(stderr, "pthread create failed\n");
    pthread_detach(thr);
  }
  
  if(close(lfd))
    perror("close lfd");    
  
  destroy_cache();
  
  return 0;
}
 
/* create and return an array of struct lines */
struct line **construct_cache(){
  struct line **c = malloc(sizeof(struct line *) * 10);
  
  int i;
  for(i = 0; i < 10; i++){
    c[i] = malloc(sizeof(struct line));
    c[i]->valid = 0;
    c[i]->age = 0;
  }
  return c;
}
 
 
/* search the cache and return the response for a given request
   returns NULL if event of cache miss */
char *search_cache(char *request){
  int i;
  int found = 0;
  char *response;
  for(i = 0; i < 10; i++){
    cache[i]->age++;
    
    if(cache[i]->valid && !strncmp(request, cache[i]->tag, strlen(request))){
      cache[i]->age = 0;
      found = 1;
      response = cache[i]->buff;
    }
  }
  if(!found){
    return NULL;
  }
  return response;
}
 
/* write request and response to cache */
void store(char *request, char *buff){
  int invalid_found = 0;
  struct line *to_replace;
  
  //find invalid line
  int i;
  for(i = 0; i < 10; i++){
    if(!cache[i]->valid){
      invalid_found = 1;
      to_replace = cache[i];
      break;
    }
  }
  
  //no invalid blocks: chose one to evict
  if(!invalid_found){
    to_replace = cache[0];
    for(i = 1; i < 10; i++){
      if(to_replace->age < cache[i]->age){
	to_replace = cache[i];
      }
    }
  }
  
  //store at chosen line
  to_replace->valid = 1;
  to_replace->age = 0;
  to_replace->tag = realloc(to_replace->tag, sizeof(char) * strlen(request));
  strncpy(to_replace->tag, request, strlen(request));
  to_replace->buff = realloc(to_replace->buff, sizeof(char) * strlen(buff));
  strncpy(to_replace->buff, buff, strlen(buff));
}
 
/* free all memory associated with cache */
void destroy_cache(){
  int i;
  for(i = 0; i < 10; i++){
    free(cache[i]->tag);
    free(cache[i]->buff);
    free(cache[i]);
  }
  
  free(cache);
}
