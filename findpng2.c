#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>

#include <pthread.h>
#include <search.h>
#include <semaphore.h>


#define SEED_URL "http://ece252-1.uwaterloo.ca/lab4"
#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 1048576  /* 1024*1024 = 1M */
#define BUF_INC  524288   /* 1024*512  = 0.5M */

#define CT_PNG  "image/png"
#define CT_HTML "text/html"
#define CT_PNG_LEN  9
#define CT_HTML_LEN 9

typedef struct arr {
    char data[2000][1024]; //array of char pointers (strings)
    size_t cap; //capacity of the list
    size_t size;    //current size 
} LIST;

LIST *found_urls; //valid png urls that have been found
LIST *visited_urls; //urls that have been visited already (PNGS and HTML)
LIST *urls_frontier; //urls that need to be visited

int curr_index = 0;

int pngs_found = 0; //counter for current pngs found

int t = 1; //number of threads
int m = 50; //max number of pngs

sem_t stack_mutex;    //

int readers = 0;

sem_t reader_mutex;
sem_t turnstile;   //
sem_t roomEmpty;  //

sem_t png_mutex;

int waiting_threads = 0;

pthread_cond_t all_waiting;
pthread_mutex_t wait_mutex;

sem_t to_visit;

//helper function to init list
void init_list(LIST *p_list){
    p_list->size = 0;
    p_list->cap =  1024;
}

//helper function to search list for given value
int search_list(LIST *p_list, const char *string){
    for (int i=0; i<p_list->size; i++){
        if (strcmp(p_list->data[i], string) == 0){
            return 1;
        }
    }

    return 0;
}

//helper function to add value to list
void add_list(LIST *p_list, const char *string){
    strcpy(p_list->data[p_list->size], string);
    p_list->size = p_list->size + 1;
}

//helper function to pop item off stack
int pop(LIST *p_list, char *p_item)
{   
    strcpy( p_item, p_list->data[p_list->size-1]);
    strcpy(p_list->data[p_list->size-1], "");
    p_list->size = p_list->size - 1;
}

#define max(a, b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

typedef struct recv_buf2 {
    char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
                     /* <0 indicates an invalid seq number */
} RECV_BUF;

typedef struct png_found {
    char url[1024];
    char *buf;
    size_t max_size;
    size_t size;
} CURL_DATA;

CURL_DATA all_data[2000];

htmlDocPtr mem_getdoc(char *buf, int size, const char *url);
xmlXPathObjectPtr getnodeset (xmlDocPtr doc, xmlChar *xpath);
int find_http(char *fname, int size, int follow_relative_links, const char *base_url);
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);
size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata);
int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);
void cleanup(CURL *curl, RECV_BUF *ptr);
int write_file(const char *path, const void *in, size_t len);
CURL *easy_handle_init(RECV_BUF *ptr, const char *url);
int process_data(CURL *curl_handle, RECV_BUF *p_recv_buf);


htmlDocPtr mem_getdoc(char *buf, int size, const char *url)
{
    int opts = HTML_PARSE_NOBLANKS | HTML_PARSE_NOERROR | \
               HTML_PARSE_NOWARNING | HTML_PARSE_NONET;
    htmlDocPtr doc = htmlReadMemory(buf, size, url, NULL, opts);
    
    if ( doc == NULL ) {
        //fprintf(stderr, "Document not parsed successfully.\n");
        return NULL;
    }
    return doc;
}

xmlXPathObjectPtr getnodeset (xmlDocPtr doc, xmlChar *xpath)
{
	
    xmlXPathContextPtr context;
    xmlXPathObjectPtr result;

    context = xmlXPathNewContext(doc);
    if (context == NULL) {
        //printf("Error in xmlXPathNewContext\n");
        return NULL;
    }
    result = xmlXPathEvalExpression(xpath, context);
    xmlXPathFreeContext(context);
    if (result == NULL) {
        //printf("Error in xmlXPathEvalExpression\n");
        return NULL;
    }
    if(xmlXPathNodeSetIsEmpty(result->nodesetval)){
        xmlXPathFreeObject(result);
        //printf("No result\n\n");
        return NULL;
    }
    return result;
}

int find_http(char *buf, int size, int follow_relative_links, const char *base_url)
{
    CURL *curl_handle;
    CURLcode res;
    long response_code;

    int i;
    htmlDocPtr doc;
    xmlChar *xpath = (xmlChar*) "//a/@href";
    xmlNodeSetPtr nodeset;
    xmlXPathObjectPtr result;
    xmlChar *href;
		
    if (buf == NULL) {
        return 1;
    }

    doc = mem_getdoc(buf, size, base_url);
    result = getnodeset (doc, xpath);
    if (result) {
        nodeset = result->nodesetval;
        for (i=0; i < nodeset->nodeNr; i++) {
            href = xmlNodeListGetString(doc, nodeset->nodeTab[i]->xmlChildrenNode, 1);
            if ( follow_relative_links ) {
                xmlChar *old = href;
                href = xmlBuildURI(href, (xmlChar *) base_url);
                xmlFree(old);
            }

            if ( href != NULL && !strncmp((const char *)href, "http", 4) ) {
                //check if visited -> if not add to URL frontier

                /*using readers-writers algo*/

                //going through turnstile
                sem_wait(&turnstile);
                sem_post(&turnstile);

                //entering readers mutex
                sem_wait(&reader_mutex);
                readers+=1;
                if (readers == 1){
                    sem_wait(&roomEmpty);
                }
                sem_post(&reader_mutex);

                //searching if url has been visited
                int searched = search_list(visited_urls, href);
                sem_wait(&reader_mutex);
                readers-=1;
                if (readers == 0){
                    sem_post(&roomEmpty);
                }
                sem_post(&reader_mutex);
 
                //if url wasn't found and still haven't met PNG limit
                if (searched != 1 && pngs_found < m){
                    //going through turnstile
                    sem_wait(&turnstile);
                    sem_wait(&roomEmpty);
                    add_list(visited_urls, href);
                    sem_post(&turnstile);
                    sem_post(&roomEmpty);

                    RECV_BUF recv_buf;

                    curl_global_init(CURL_GLOBAL_DEFAULT);                   
                    curl_handle = easy_handle_init(&recv_buf, href);

                    /* get it! */
                    res = curl_easy_perform(curl_handle);

                    if( res == CURLE_OK) {
                        //get response code 
                        res = curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);

                        //get type
                        char *ct = NULL;
                        res = curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_TYPE, &ct);

                        if ( strstr(ct, CT_HTML) ) {
                            //add to url frontier, push to stack
                            sem_wait(&stack_mutex);
                            sem_wait(&png_mutex);
                            if (pngs_found < m){
                                add_list(urls_frontier, href);
                                sem_post(&to_visit);
                            }
                            sem_post(&png_mutex);
                            
                            all_data[curr_index].buf = recv_buf.buf;
                            all_data[curr_index].size = recv_buf.size;
                            all_data[curr_index].max_size = recv_buf.max_size;

                            strcpy(all_data[curr_index].url, href);
                            curr_index +=1;

                            sem_post(&stack_mutex);

                            curl_easy_cleanup(curl_handle);
                            curl_global_cleanup();

                        } else if ( strstr(ct, CT_PNG) ) {
                            //add to PNG list if its a valid PNG
                            if (isValidPNG(recv_buf) == 1){
                                sem_wait(&png_mutex);
                                if (pngs_found < m){
                                    add_list(found_urls, href);
                                    pngs_found += 1;
                                }
                                sem_post(&png_mutex);
                            }
                            cleanup(curl_handle, &recv_buf);
                        } else {
                            cleanup(curl_handle, &recv_buf);
                        }

                    } else {
                       cleanup(curl_handle, &recv_buf);
                    }
                } 

            }

            xmlFree(href);
        }
        xmlXPathFreeObject (result);
    }
    xmlFreeDoc(doc);
    return 0;
}

/**
 * @brief  cURL header call back function to extract image sequence number from 
 *         http header data. An example header for image part n (assume n = 2) is:
 *         X-Ece252-Fragment: 2
 * @param  char *p_recv: header data delivered by cURL
 * @param  size_t size size of each memb
 * @param  size_t nmemb number of memb
 * @param  void *userdata user defined data structurea
 * @return size of header data received.
 * @details this routine will be invoked multiple times by the libcurl until the full
 * header data are received.  we are only interested in the ECE252_HEADER line 
 * received so that we can extract the image sequence number from it. This
 * explains the if block in the code.
 */
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;

#ifdef DEBUG1_
    //printf("%s", p_recv);
#endif /* DEBUG1_ */
    if (realsize > strlen(ECE252_HEADER) &&
	strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0) {

        /* extract img sequence number */
	p->seq = atoi(p_recv + strlen(ECE252_HEADER));

    }
    return realsize;
}

size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata)
{
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;
 
    if (p->size + realsize + 1 > p->max_size) {/* hope this rarely happens */ 
        /* received data is not 0 terminated, add one byte for terminating 0 */
        size_t new_size = p->max_size + max(BUF_INC, realsize + 1);   
        char *q = realloc(p->buf, new_size);
        if (q == NULL) {
            perror("realloc"); /* out of memory */
            return -1;
        }
        p->buf = q;
        p->max_size = new_size;
    }

    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}


/**
 * @brief write callback function to save a copy of received data in RAM.
 *        The received libcurl data are pointed by p_recv, 
 *        which is provided by libcurl and is not user allocated memory.
 *        The user allocated memory is at p_userdata. One needs to
 *        cast it to the proper struct to make good use of it.
 *        This function maybe invoked more than once by one invokation of
 *        curl_easy_perform().
 */



int recv_buf_init(RECV_BUF *ptr, size_t max_size)
{
    void *p = NULL;
    
    if (ptr == NULL) {
        return 1;
    }

    p = malloc(max_size);

    if (p == NULL) {
	return 2;
    }
    
    ptr->buf = p;
    ptr->size = 0;
    ptr->max_size = max_size;
    ptr->seq = -1;              /* valid seq should be positive */
    return 0;
}

int recv_buf_cleanup(RECV_BUF *ptr)
{
    if (ptr == NULL) {
	return 1;
    }
    
    free(ptr->buf);
    ptr->size = 0;
    ptr->max_size = 0;
    return 0;
}

void cleanup(CURL *curl, RECV_BUF *ptr)
{
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    recv_buf_cleanup(ptr);

    return NULL;
}

//helper function to check if object is a valid PNG
int isValidPNG(RECV_BUF recv_buf){
    //decalring variables
    char *buf = recv_buf.buf;
    char signature[] = {0x50, 0x4E, 0x47};

    //byteshift by 1
    buf++;

    //checking if values are the same
    int response = 1;
    for(int i=0; i<3; i++){
        if (buf[i] != signature[i]){
            response = -1;
            break;
        }
    }

    return response;
}

CURL *easy_handle_init(RECV_BUF *ptr, const char *url)
{
    CURL *curl_handle = NULL;

    if ( ptr == NULL || url == NULL) {
        return NULL;
    }

    /* init user defined call back function buffer */
    if ( recv_buf_init(ptr, BUF_SIZE) != 0 ) {
        return NULL;
    }
    /* init a curl session */
    curl_handle = curl_easy_init();

    if (curl_handle == NULL) {
        //fprintf(stderr, "curl_easy_init: returned NULL\n");
        return NULL;
    }

    /* specify URL to get */
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);

    /* register write call back function to process received data */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl3); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)ptr);

    /* register header call back function to process received header data */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)ptr);

    /* some servers requires a user-agent field */
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "ece252 lab4 crawler");

    /* follow HTTP 3XX redirects */
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    /* continue to send authentication credentials when following locations */
    curl_easy_setopt(curl_handle, CURLOPT_UNRESTRICTED_AUTH, 1L);
    /* max numbre of redirects to follow sets to 5 */
    curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 5L);
    /* supports all built-in encodings */ 
    curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "");

    /* Max time in seconds that the connection phase to the server to take */
    //curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 5L);
    /* Max time in seconds that libcurl transfer operation is allowed to take */
    //curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
    /* Time out for Expect: 100-continue response in milliseconds */
    //curl_easy_setopt(curl_handle, CURLOPT_EXPECT_100_TIMEOUT_MS, 0L);

    /* Enable the cookie engine without reading any initial cookies */
    curl_easy_setopt(curl_handle, CURLOPT_COOKIEFILE, "");
    /* allow whatever auth the proxy speaks */
    curl_easy_setopt(curl_handle, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
    /* allow whatever auth the server speaks */
    curl_easy_setopt(curl_handle, CURLOPT_HTTPAUTH, CURLAUTH_ANY);

    return curl_handle;
}

int search_html(CURL *curl_handle, char* buf, size_t size, const char* url)
{
    int follow_relative_link = 1;

    if (curl_handle != NULL){
        char *new_url = NULL; 
        curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &new_url);
        find_http(buf, size, follow_relative_link, new_url); 
    } else {
        find_http(buf, size, follow_relative_link, url);
    }

    return 0;
}

/**
 * @brief process teh download data by curl
 * @param CURL *curl_handle is the curl handler
 * @param RECV_BUF p_recv_buf contains the received data. 
 * @return 0 on success; non-zero otherwise
 */

int searched = 0;

void *find_urls(){
    CURL *curl_handle = NULL;
    CURLcode res;
    while (1) {
        pthread_mutex_lock(&wait_mutex);

        waiting_threads += 1;

        //if following conditions are met, post to condition variable
        if ((urls_frontier->size == 0 ||pngs_found >=m)  && waiting_threads == t ){
            pthread_cond_signal(&all_waiting);
            pthread_mutex_unlock(&wait_mutex);
            break;
        }

        pthread_mutex_unlock(&wait_mutex);

        //cancel thread if PNG limit is met
        if (pngs_found >= m){
            break;
        }

        char* url[1024];

        sem_wait(&to_visit);

        waiting_threads -=1;

        //popping from url frontier stack
        sem_wait(&stack_mutex);
        pop(urls_frontier, url);
        sem_post(&stack_mutex);

        int found_index = -1;

        //check for all curl requests
        for (int i=0; i<curr_index+1; i++){
            if (strcmp(all_data[i].url, url) == 0){
                found_index = i;
                searched+=1;
                break;
            }
        }

        //fetch data
        if (found_index != -1){
            search_html(NULL, all_data[found_index].buf, all_data[found_index].size, url);
        } else {
            RECV_BUF recv_buf;
            curl_global_init(CURL_GLOBAL_DEFAULT);
            curl_handle = easy_handle_init(&recv_buf, url);

            /* get it! */
            res = curl_easy_perform(curl_handle);        

            search_html(curl_handle, recv_buf.buf, recv_buf.size, NULL);
            cleanup(curl_handle, &recv_buf);
        }
    }
}

int main( int argc, char** argv ) {
    //declaring string variables
    char *seed_url[1024];
    char *log_name[1024];

    //declaring time variables
    double times[2];
    struct timeval tv;

    //parsing input
    t = atoi(argv[2]);
    m = atoi(argv[4]);
    if (argc == 8){
        strcpy(log_name, argv[6]);
        strcpy(seed_url, argv[7]);
    } else {
        strcpy(seed_url, argv[5]);
    }

    //starting time calculation
    if( gettimeofday(&tv, NULL) != 0 ) {
        perror("gettimeofday");
        abort();
    }
    times[0] = (tv.tv_sec) + tv.tv_usec/1000000;    
    
    //allocating memory
    found_urls = malloc(sizeof(LIST));
    visited_urls = malloc(sizeof(LIST));
    urls_frontier = malloc(sizeof(LIST));

    //calling helper function to init lists
    init_list(found_urls);
    init_list(visited_urls);
    init_list(urls_frontier);

    //init semaphores
    sem_init(&stack_mutex, 0, 1);
    sem_init(&reader_mutex, 0, 1);
    sem_init(&turnstile, 0, 1);
    sem_init(&roomEmpty, 0, 1);
    sem_init(&png_mutex, 0, 1);
    sem_init(&to_visit, 0, 1);
    pthread_mutex_init(&wait_mutex, NULL);

    //init data buffers
    for (int i=0; i < 2000; i++){
        all_data[i].buf = NULL;
    }

    //declaring thread ids
    pthread_t p_tids[t];
    
    //adding seed url to respective lists
    add_list(urls_frontier, seed_url);
    add_list(visited_urls, seed_url);

    //init xml parser
    xmlInitParser();

    //creating threads
    for(int i = 0; i < t; i++) {
        pthread_create(&p_tids[i], NULL, find_urls, NULL);
    }    

    //waiting on condition variable
    pthread_mutex_lock(&wait_mutex);
	pthread_cond_wait(&all_waiting, &wait_mutex);
	pthread_mutex_unlock(&wait_mutex);

    //cancelling threads
    for (int i = 0; i < t; i++){
        pthread_cancel(p_tids[i]);
    }

    //freeing memory
    for (int i=0; i < curr_index+1; i++){
        if (all_data[i].buf != NULL){
            free(all_data[i].buf);
            all_data[i].buf = NULL;
        }
    }

    //opening new file
    FILE *outputFile = fopen("png_urls.txt", "wb+");

    //writing to file
    for (int i = 0; i < found_urls->size; i++){
        fputs(found_urls->data[i], outputFile);
        if (i != found_urls->size-1){
            fputs("\n", outputFile);
        }
    }

    //closing file
    fclose(outputFile);

    //writing out visited urls if requested
    if (argc == 8){
        FILE *logFile = fopen(log_name, "wb+");

        for (int i = 0; i < visited_urls->size; i++){
            fputs(visited_urls->data[i], logFile);
            if (i != visited_urls->size-1){
                fputs("\n", logFile);
            }
        }

        fclose(logFile);
    }

    //finish time calculations
    if (gettimeofday(&tv, NULL) != 0) {
      perror("gettimeofday");
      abort();
    }
    times[1] = (tv.tv_sec) + tv.tv_usec/1000000.;

    //output execution time
    printf("findpng2 execution time: %.6lf seconds\n", times[1] - times[0]);

    //destroying semaphoress
    sem_destroy(&stack_mutex);
    sem_destroy(&turnstile);
    sem_destroy(&roomEmpty);
    sem_destroy(&reader_mutex);
    sem_destroy(&png_mutex);
    sem_destroy(&to_visit);
    pthread_mutex_destroy(&wait_mutex);

    //cleaning memory leaks
    free(found_urls);
    free(visited_urls);
    free(urls_frontier);

    return 0;
}