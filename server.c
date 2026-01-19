#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>


#define PORT 8080
#define WORKER_COUNT 52
#define QUEUE_SIZE 256
#define CACHE_MAX_SIZE (10 * 1024 * 1024) 
#define MAX_OBJECT_SIZE (512 * 1024)
#define DNS_CACHE_SIZE 256
#define DNS_TTL 300



void handle_clients(int client_fd);
void handle_connect_tunnel(int client_fd, char *request);



int clint_queue[QUEUE_SIZE];
int queuq_front = 0;
int queue_rear = 0;
int queue_count = 0;

pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t queue_not_full = PTHREAD_COND_INITIALIZER;


typedef struct dns_entry {
    char host[256];
    struct sockaddr_storage addr;
    socklen_t addrlen;
    time_t timestamp;
    struct dns_entry *next;
} dns_entry;



typedef struct cache_entry {
    char *key;                 
    char *data;                
    size_t size;

    struct cache_entry *prev;
    struct cache_entry *next;
} cache_entry;

dns_entry *dns_cache = NULL;
pthread_mutex_t dns_mutex = PTHREAD_MUTEX_INITIALIZER;


cache_entry *cache_head = NULL;
cache_entry *cache_tail = NULL;

size_t cache_current_size = 0;

pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

int dns_cache_lookup(const char *host,struct sockaddr_storage *out,socklen_t *outlen){
    time_t now = time(NULL);
    pthread_mutex_lock(&dns_mutex);

    for(dns_entry *e = dns_cache; e; e=e->next){
        if(strcmp(e->host,host)==0 && now - e->timestamp < DNS_TTL){
            memcpy(out,&e->addr,e->addrlen);
            *outlen = e->addrlen;
            pthread_mutex_unlock(&dns_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&dns_mutex);
    return 0;
}

void dns_cache_store(const char *host,struct sockaddr *addr,socklen_t addrlen){
    dns_entry *e = malloc(sizeof(dns_entry));
    strcpy(e->host,host);
    memcpy(&e->addr,addr,addrlen);
    e->addrlen = addrlen;
    e->timestamp = time(NULL);

    pthread_mutex_lock(&dns_mutex);
    e->next = dns_cache;
    dns_cache = e;
    pthread_mutex_unlock(&dns_mutex);
}

void cache_move_to_front(cache_entry *e){ // entry
    if(e == cache_head){
        return;
    }
    if(e->prev){
        e->prev->next = e->next;
    }

    if(e->next){
        e->next->prev = e->prev;
    }

    if(e== cache_tail){
        cache_tail = e->prev;
    }

    e->prev = NULL;
    e->next = cache_head;

    if(cache_head){
        cache_head->prev = e;
    }
    
    cache_head = e;

    if(!cache_tail){
        cache_tail = e;
    }
}


void cache_evict(){
    if(!cache_tail){
        return;
    }
    cache_entry *e = cache_tail;

    if(e->prev){
        e->prev->next = NULL;
    }
    cache_tail = e->prev;

    if(e==cache_head){
        cache_head = NULL;
    }
    cache_current_size -= e->size;
    free(e->key);
    free(e->data);
    free(e); 
}

cache_entry *cache_get(const char *key){
    pthread_mutex_lock(&cache_mutex);

    cache_entry *e = cache_head;
    while(e){
        if(strcmp(e->key,key)==0){
            cache_move_to_front(e);
            pthread_mutex_unlock(&cache_mutex);
            return e;
        }
        e = e->next;
    }
    pthread_mutex_unlock(&cache_mutex);
    return NULL;
}


void cache_put(const char *key,char *data,size_t size){
    if(size>MAX_OBJECT_SIZE){
        return;
    }

    pthread_mutex_lock(&cache_mutex);

    while(cache_current_size + size >CACHE_MAX_SIZE){
        cache_evict();
    }

    cache_entry *e = malloc(sizeof(cache_entry));
    e->key = strdup(key);
    e->data = data;
    e->size = size;

    e->prev = NULL;
    e->next = cache_head;

    if(cache_head){
        cache_head->prev = e;
    }
    cache_head = e;

    if(!cache_tail){
        cache_tail = e;
    }

    cache_current_size +=size;

    pthread_mutex_unlock(&cache_mutex);

}


void enque_client(int client_fd){
    pthread_mutex_lock(&queue_mutex);

    while(queue_count == QUEUE_SIZE){
        pthread_cond_wait(&queue_not_full,&queue_mutex);
    }
    clint_queue[queue_rear] = client_fd;
    queue_rear = (queue_rear+1) % QUEUE_SIZE;
    queue_count++;

    pthread_cond_signal(&queue_not_empty);
    pthread_mutex_unlock(&queue_mutex);
}


int dequeue_client(){
    pthread_mutex_lock(&queue_mutex);

    while(queue_count == 0){
        pthread_cond_wait(&queue_not_empty,&queue_mutex);
    }

    int client_fd = clint_queue[queuq_front];
    queuq_front = (queuq_front + 1) % QUEUE_SIZE;
    queue_count--;

    pthread_cond_signal(&queue_not_full);
    pthread_mutex_unlock(&queue_mutex);

    return client_fd;
}


void *worker_thread(void *args){
    (void)args;

    while(1){
        int client_fd = dequeue_client();
        handle_clients(client_fd);
    }
    return NULL;
}

int split_host_port(char *host,int *port){
    char *colon = strchr(host,':');
    if(colon){
        *colon = '\0';
        *port = atoi(colon+1);
        if(*port <=0){
            *port = 80;
        }
    }
    else{
        *port=80;
    }
    return 0;
}

void extract_host(const char *request,char *host){
    const char *host_start = strstr(request,"Host:");
    if(!host_start){
        host[0]='\0';
        return;
    }

    host_start +=5;
    while(*host_start == ' '){
        host_start++;
    }

    const char *host_end = strstr(host_start,"\r\n");
    if(!host_end){
        host[0]='\0';
        return;
    }
    int len = host_end - host_start;
    strncpy(host,host_start,len);
    host[len]='\0';
}

void normalise_request(char *request){
    char methrod[16],url[2048],protocol[16];

    if(sscanf(request,"%15s %2047s %15s",methrod,url,protocol)!=3){
        return;
    }

    if(strncmp(url,"http://",7)==0){
        char *path = strchr(url+7,'/');
        if(!path){
            path = "/";
        } 
        char new_request[4096];

        snprintf(new_request,sizeof(new_request),"%s %s %s\r\n",methrod,path,protocol);

        char *rest = strstr(request,"\r\n");

        if(!rest){
            return;
        }

        memmove(request,new_request,strlen(new_request));
        memmove(request +strlen(new_request),rest+2,strlen(rest+2)+1);

    }
}

void handle_connect_tunnel(int client_fd, char *request) {
    char method[16], target[256], protocol[16];
    char host[256];
    int port;

    if (sscanf(request, "%15s %255s %15s", method, target, protocol) != 3) {
        close(client_fd);
        return;
    }

    strcpy(host, target);
    split_host_port(host, &port);

    printf("CONNECTED TUNNEL to %s:%d\n", host, port);

    // Thread-safe DNS
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;        // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        close(client_fd);
        return;
    }

    int remote_fd = socket(res->ai_family,
                           res->ai_socktype,
                           res->ai_protocol);
    if (remote_fd < 0) {
        freeaddrinfo(res);
        close(client_fd);
        return;
    }

    if (connect(remote_fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(client_fd);
        close(remote_fd);
        return;
    }

    freeaddrinfo(res);

    const char *ok =
        "HTTP/1.1 200 Connection Established\r\n"
        "Proxy-Agent: C-Proxy\r\n"
        "\r\n";

    write(client_fd, ok, strlen(ok));

    fd_set fds;
    char buf[8192];

    while (1) {
        FD_ZERO(&fds);
        FD_SET(client_fd, &fds);
        FD_SET(remote_fd, &fds);

        int maxfd = (client_fd > remote_fd ? client_fd : remote_fd) + 1;

        struct timeval tv = {30, 0};
        int ready = select(maxfd, &fds, NULL, NULL, &tv);
        if (ready <= 0) break;

        if (FD_ISSET(client_fd, &fds)) {
            int n = recv(client_fd, buf, sizeof(buf), 0);
            if (n <= 0 || send(remote_fd, buf, n, 0) <= 0) break;
        }

        if (FD_ISSET(remote_fd, &fds)) {
            int n = recv(remote_fd, buf, sizeof(buf), 0);
            if (n <= 0 || send(client_fd, buf, n, 0) <= 0) break;
        }
    }

    close(client_fd);
    close(remote_fd);
}


void handle_clients(int client_fd) {
    char buffer[4096];
    char host[256];
    char cache_key[1024];
    int port;

    int bytes = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes <= 0) {
        close(client_fd);
        return;
    }

    buffer[bytes] = '\0';
    printf("----REQUEST ----\n%s\n----------------------\n", buffer);


    char method[16], path[2048], protocol[16];

    if (sscanf(buffer, "%15s %2047s %15s", method, path, protocol) != 3) {
    close(client_fd);
    return;
   }


    extract_host(buffer, host);
    normalise_request(buffer);
    split_host_port(host, &port);

    int cacheable = (strcmp(method, "GET") == 0 && port == 80);

    snprintf(cache_key, sizeof(cache_key), "%.255s%.768s", host, path);

    if (cacheable) {
    cache_entry *cached = cache_get(cache_key);
    if (cached) {
        printf("CACHE HIT: %s\n", cache_key);
        write(client_fd, cached->data, cached->size);
        close(client_fd);
        return;
    }
    else{
        printf("CACHE MISS: %s\n", cache_key);
    }
 }

    printf("Host: %s | Port: %d\n", host, port);

    struct addrinfo hints, *res = NULL;
    int used_getaddrinfo = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    // dns cache 

    struct sockaddr_storage addr;
    socklen_t addrlen;

    if(!dns_cache_lookup(host,&addr,&addrlen)){
        memset(&hints,0,sizeof(hints));
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        close(client_fd);
        return;
       }

       memcpy(&addr,res->ai_addr,res->ai_addrlen);
       addrlen = res->ai_addrlen;

       dns_cache_store(host,res->ai_addr,res->ai_addrlen);
       used_getaddrinfo = 1;

    }

    

    int remote_fd = socket(addr.ss_family,SOCK_STREAM,0);
    if (remote_fd < 0) {  
        if(used_getaddrinfo){
        freeaddrinfo(res);
    }
        close(client_fd);
        return;
    }

    if (connect(remote_fd, (struct sockaddr *)&addr,addrlen) < 0) {
        if(used_getaddrinfo){
        freeaddrinfo(res);
    }
        close(client_fd);
        close(remote_fd);
        return;
    }

    if(used_getaddrinfo){
        freeaddrinfo(res);
    }

    

    char *pc;
    pc = strstr(buffer, "Proxy-Connection:");
    if (pc) {
        char *end = strstr(pc, "\r\n");
        if (end) memmove(pc, end + 2, strlen(end + 2) + 1);
    }

    char *conn = strstr(buffer, "Connection:");
    if (conn) {
    char *end = strstr(conn, "\r\n");
    if (end) memmove(conn, end + 2, strlen(end + 2) + 1);
   }


   strcat(buffer, "Connection: close\r\n");


    write(remote_fd, buffer, strlen(buffer));

    char response_buf[MAX_OBJECT_SIZE];
    size_t total_bytes = 0;

    int n;
    while ((n = read(remote_fd,
                 response_buf + total_bytes,
                 sizeof(response_buf) - total_bytes)) > 0) {

    write(client_fd,
          response_buf + total_bytes,
          n);

    total_bytes += n;

    if (total_bytes >= MAX_OBJECT_SIZE)
        break;
    }

    if (cacheable && total_bytes < MAX_OBJECT_SIZE) {
    char *copy = malloc(total_bytes);
    memcpy(copy, response_buf, total_bytes);
    cache_put(cache_key, copy, total_bytes);
   }



    close(client_fd);
    close(remote_fd);
}

void *connect_thread(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    char buffer[4096];
    int n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        close(client_fd);
        return NULL;
    }

    buffer[n] = '\0';
    handle_connect_tunnel(client_fd, buffer);
    return NULL;
}



int main(){
    int lisen_fd;

    struct sockaddr_in server_addr,clint_addr;

    socklen_t addr_len = sizeof(clint_addr);


    // now will create socket 
    lisen_fd = socket(AF_INET, SOCK_STREAM , 0);
    if(lisen_fd<0){
        perror("Socket failed");
        exit(1);
    }


    int opt = 1;
    setsockopt(lisen_fd,SOL_SOCKET, SO_REUSEADDR,&opt,sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    

    if(bind(lisen_fd,(struct sockaddr*)&server_addr,sizeof(server_addr))<0){
        perror("Bind failed");
        close(lisen_fd);
        exit(1);
    }

    // listen

    if(listen(lisen_fd,5)<0){
        perror("Listen failed");
        close(lisen_fd);
        exit(1);
    }

    printf("Proxy server running on port: %d ",PORT);
    fflush(stdout);


    pthread_t workers[WORKER_COUNT];
    for(int i=0;i<WORKER_COUNT;i++){
        pthread_create(&workers[i],NULL,worker_thread,NULL);
    }

    signal(SIGPIPE, SIG_IGN);


    while(1){
        int client_fd = accept(lisen_fd,(struct sockaddr *)&clint_addr,&addr_len);

        if(client_fd<0){
            perror("Accept failed");
            continue;
        }

        char peekbuf[16];
        int n = recv(client_fd,peekbuf,sizeof(peekbuf)-1,MSG_PEEK);
        if(n<=0){
            close(client_fd);
            continue;
        }
        peekbuf[n]='\0';

        if(strncmp(peekbuf,"CONNECT",7)==0){
            pthread_t tid;
            int *fd = malloc(sizeof(int));
            *fd = client_fd;

            pthread_create(&tid,NULL,connect_thread,fd);
            pthread_detach(tid);
        }
        else{
            enque_client(client_fd);
        }
        
    }

    close(lisen_fd);
    return 0;
}