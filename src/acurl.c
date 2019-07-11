#include "ae/ae.h"
#include <curl/multi.h>
#include <Python.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <stdbool.h>
#include "structmember.h"

#define NO_ACTIVE_TIMER_ID -1

/* Macro for debugging */

#define DEBUG 0
#if defined(DEBUG) && DEBUG > 0
    #include <sys/syscall.h>
    #define DEBUG_PRINT(fmt, args...) fprintf(stderr, "DEBUG: %s:%d:%s() tid=%ld: " fmt "\n", __FILE__, __LINE__, __func__, (long)syscall(SYS_gettid), ##args)
#else
    #define DEBUG_PRINT(fmt, args...)
#endif

#include <time.h>
static inline double gettime(void) {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    return ((double)tp.tv_sec) + ((double)tp.tv_nsec  / 1000000000.0);
}

/* For finding memory used by the program */

static inline int getmem(void) {
    int mem;
    FILE *f = fopen("/proc/self/statm", "rb");
    fscanf(f, "%*d %d", &mem);
    fclose(f);
    return mem * 4096;
}

/* Macro for debugging */

#define REQUEST_TRACE 0
#if defined(REQUEST_TRACE) && REQUEST_TRACE > 0
    #include <sys/syscall.h>
    #define REQUEST_TRACE_PRINT(location, pointer) fprintf(stderr, "%s %p %f\n", location, pointer, gettime())
#else
    #define REQUEST_TRACE_PRINT(location, pointer) /* Don't do anything in release builds */
#endif

/* Can be enabled to trace time or memory usage*/

#define PROFILE 0
#if defined(PROFILE) && PROFILE == 1
    #include <sys/syscall.h>
    #define ENTER() fprintf(stderr, "ENTER %ld:%s:%d:%s %.9f\n", (long)syscall(SYS_gettid), __FILE__, __LINE__, __func__, gettime())
    #define EXIT() fprintf(stderr, "EXIT %ld:%s:%d:%s %.9f\n", (long)syscall(SYS_gettid), __FILE__, __LINE__, __func__, gettime())
#elif defined(PROFILE) && PROFILE == 2
    #include <sys/syscall.h>
    #define ENTER() fprintf(stderr, "ENTER %s:%d:%s %d\n", __FILE__, __LINE__, __func__, getmem())
    #define EXIT() fprintf(stderr, "EXIT %s:%d:%s %d\n", __FILE__, __LINE__, __func__, getmem())
#else
    #define ENTER()
    #define EXIT()
#endif

/* Used to tell gcc about expected code branches, likely not necessary */

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

typedef struct {
    PyObject_HEAD
    aeEventLoop *event_loop;
    PyThreadState* thread_state;
    CURLM *multi;
    long long timer_id;
    bool stop;
    int req_in_read;
    int req_in_write;
    int req_out_read;
    int req_out_write;
    int stop_read;
    int stop_write;
    int curl_easy_cleanup_read;
    int curl_easy_cleanup_write;
} EventLoop;


typedef struct {
    PyObject_HEAD
    EventLoop *loop;
    CURLSH *shared;
} Session;

/* Node in a linked list structure. Used for piecing together sections of resposnes e.g. headers and body.
 * Possible optimisation to have a memory pool for buffer nodes so they aren't being malloc'ed all the time */

struct BufferNode {
    int len;
    char *buffer;
    struct BufferNode *next;
};


typedef struct {
    char* method;
    char* url;
    char* auth;
    PyObject* cookies;
    int cookies_len;
    char** cookies_str;
    PyObject* future;
    struct curl_slist* headers;
    int req_data_len;
    char* req_data_buf;
    Session* session;
    CURL *curl;
    CURLcode result;
    struct BufferNode *header_buffer_head;
    struct BufferNode *header_buffer_tail;
    struct BufferNode *body_buffer_head;
    struct BufferNode *body_buffer_tail;
    int dummy;
    char* ca_cert;
    char* ca_key;
} AcRequestData;


typedef struct {
    PyObject_HEAD
    struct BufferNode *header_buffer;
    struct BufferNode *body_buffer;
    Session *session;
    CURL *curl;
} Response;


void free_buffer_nodes(struct BufferNode *start) {
    ENTER();
    struct BufferNode *node = start;
    while(node != NULL)
    {
        struct BufferNode *next = node->next;
        free(node->buffer);
        free(node);
        node = next;
    };
    EXIT();
}

/* Python deallocator for Response Object. For GC */

static void Response_dealloc(Response *self)
{
    ENTER();
    DEBUG_PRINT("response=%p", self);
    free_buffer_nodes(self->header_buffer);
    free_buffer_nodes(self->body_buffer);
    write(self->session->loop->curl_easy_cleanup_write, &self->curl, sizeof(CURL *));
    Py_XDECREF(self->session);
    Py_TYPE(self)->tp_free((PyObject*)self);
    EXIT();
}


PyObject * get_buffer_as_pylist(struct BufferNode *start)
{
    ENTER();
    int i = 0, len = 0;
    PyObject* list;
    struct BufferNode *node = start;
    while(node != NULL)
    {
        len++;
        node = node->next;
    };
    list = PyList_New(len);
    node = start;;
    while(node != NULL)
    {
        PyList_SET_ITEM(list, i++, PyBytes_FromStringAndSize(node->buffer, node->len));
        node = node->next;
    };
    DEBUG_PRINT("list=%p", list);
    EXIT();
    return list;
}


static PyObject *
Response_get_header(Response *self, PyObject *args)
{
    ENTER();
    DEBUG_PRINT("");
    PyObject *rtn = get_buffer_as_pylist(self->header_buffer);
    EXIT();
    return rtn;
}


static PyObject *
Response_get_body(Response *self, PyObject *args)
{
    ENTER();
    DEBUG_PRINT("");
    PyObject *rtn = get_buffer_as_pylist(self->body_buffer);
    EXIT();
    return rtn;
}


PyObject *resp_get_info_long(Response *self, CURLINFO info)
{
    ENTER();
    long value;
    curl_easy_getinfo(self->curl, info, &value);
    PyObject *rtn = PyLong_FromLong(value);
    EXIT();
    return rtn;
}

PyObject *resp_get_info_double(Response *self, CURLINFO info)
{
    ENTER();
    double value;
    curl_easy_getinfo(self->curl, info, &value);
    PyObject *rtn = PyFloat_FromDouble(value);
    EXIT();
    return rtn;
}

PyObject *resp_get_info_unicode(Response *self, CURLINFO info)
{
    ENTER();
    char *value = NULL;
    curl_easy_getinfo(self->curl, info, &value);
    PyObject *rtn;
    if(value != NULL) {
        rtn = PyUnicode_FromString(value);
    }
    else {
        Py_INCREF(Py_None);
        rtn = Py_None;
    }
    EXIT();
    return rtn;
}

static PyObject *Response_get_effective_url(Response *self, PyObject *args)
{
    ENTER();
    PyObject *rtn = resp_get_info_unicode(self, CURLINFO_EFFECTIVE_URL);
    EXIT();
    return rtn;
}

static PyObject *Response_get_response_code(Response *self, PyObject *args)
{
    ENTER();
    PyObject *rtn = resp_get_info_long(self, CURLINFO_RESPONSE_CODE);
    EXIT();
    return rtn;
}

static PyObject *Response_get_total_time(Response *self, PyObject *args)
{
    ENTER();
    PyObject *rtn = resp_get_info_double(self, CURLINFO_TOTAL_TIME);
    EXIT();
    return rtn;
}

static PyObject *Response_get_namelookup_time(Response *self, PyObject *args)
{
    ENTER();
    PyObject *rtn = resp_get_info_double(self, CURLINFO_NAMELOOKUP_TIME);
    EXIT();
    return rtn;
}

static PyObject *Response_get_connect_time(Response *self, PyObject *args)
{
    ENTER();
    PyObject *rtn = resp_get_info_double(self, CURLINFO_CONNECT_TIME);
    EXIT();
    return rtn;
}

static PyObject *Response_get_appconnect_time(Response *self, PyObject *args)
{
    ENTER();
    PyObject *rtn = resp_get_info_double(self, CURLINFO_APPCONNECT_TIME);
    EXIT();
    return rtn;
}

static PyObject *Response_get_pretransfer_time(Response *self, PyObject *args)
{
    ENTER();
    PyObject *rtn = resp_get_info_double(self, CURLINFO_PRETRANSFER_TIME);
    EXIT();
    return rtn;
}

static PyObject *Response_get_starttransfer_time(Response *self, PyObject *args)
{
    ENTER();
    PyObject *rtn = resp_get_info_double(self, CURLINFO_STARTTRANSFER_TIME);
    EXIT();
    return rtn;
}

static PyObject *Response_get_size_upload(Response *self, PyObject *args)
{
    ENTER();
    PyObject *rtn = resp_get_info_double(self, CURLINFO_SIZE_UPLOAD);
    EXIT();
    return rtn;
}

static PyObject *Response_get_size_download(Response *self, PyObject *args)
{
    ENTER();
    PyObject *rtn = resp_get_info_double(self, CURLINFO_SIZE_DOWNLOAD);
    EXIT();
    return rtn;
}

static PyObject *Response_get_primary_ip(Response *self, PyObject *args)
{
    ENTER();
    PyObject *rtn = resp_get_info_unicode(self, CURLINFO_PRIMARY_IP);
    EXIT();
    return rtn;
}

static PyObject *Response_get_cookielist(Response *self, PyObject *args)
{
    ENTER();
    struct curl_slist *start = NULL;
    struct curl_slist *node = NULL;
    int len = 0; int i = 0;
    PyObject *list = NULL;
    curl_easy_getinfo(self->curl, CURLINFO_COOKIELIST, &start);
    node = start;
    while(node != NULL) {
        len++;
        node = node->next;
    }
    list = PyList_New(len);
    node = start;
    while(node != NULL)
    {
        PyList_SET_ITEM(list, i++, PyUnicode_FromString(node->data));
        node = node->next;
    };
    curl_slist_free_all(start);
    EXIT();
    return list;
}

static PyObject *Response_get_redirect_url(Response *self, PyObject *args)
{
    ENTER();
    PyObject *rtn = resp_get_info_unicode(self, CURLINFO_REDIRECT_URL);
    EXIT();
    return rtn;
}



static PyMethodDef Response_methods[] = {
    {"get_effective_url", (PyCFunction)Response_get_effective_url, METH_NOARGS, ""},
    {"get_response_code", (PyCFunction)Response_get_response_code, METH_NOARGS, ""},
    {"get_total_time", (PyCFunction)Response_get_total_time, METH_NOARGS, ""},
    {"get_namelookup_time", (PyCFunction)Response_get_namelookup_time, METH_NOARGS, "Gets elapsed time from start of request to when DNS was resolved in seconds"},
    {"get_connect_time", (PyCFunction)Response_get_connect_time, METH_NOARGS, "Get elapsed time from start of request to TCP connect in seconds"},
    {"get_appconnect_time", (PyCFunction)Response_get_appconnect_time, METH_NOARGS, "Get elapsed time from start of request to TLS/SSL negotioation complete in seconds"},
    {"get_pretransfer_time", (PyCFunction)Response_get_pretransfer_time, METH_NOARGS, "Get elapsed time from start of request we've started to send the request"},
    {"get_starttransfer_time", (PyCFunction)Response_get_starttransfer_time, METH_NOARGS, "Get elapsed time from start of request until the first byte is recieved in seconds"},
    {"get_size_upload", (PyCFunction)Response_get_size_upload, METH_NOARGS, ""},

    {"get_size_download", (PyCFunction)Response_get_size_download, METH_NOARGS, ""},
    {"get_primary_ip", (PyCFunction)Response_get_primary_ip, METH_NOARGS, ""},
    {"get_cookielist", (PyCFunction)Response_get_cookielist, METH_NOARGS, ""},
    {"get_redirect_url", (PyCFunction)Response_get_redirect_url, METH_NOARGS, "Get the redirect URL or None"},
    {"get_header", (PyCFunction)Response_get_header, METH_NOARGS, "Get the header"},
    {"get_body", (PyCFunction)Response_get_body, METH_NOARGS, "Get the body"},
    {NULL, NULL, 0, NULL}
};


static PyMemberDef Response_members[] = {
    {NULL}
};


static PyTypeObject ResponseType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_acurl.Response",           /* tp_name */
    sizeof(Response),           /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)Response_dealloc,           /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "Response Type",           /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    Response_methods,          /* tp_methods */
    Response_members,          /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    0,                         /* tp_new */
};

/* When at least one request has completed, write completed responses onto completion queue*/

void response_complete(EventLoop *loop)
{
    ENTER();
    DEBUG_PRINT("loop=%p", loop);
    int remaining_in_queue = 1;
    AcRequestData *rd;
    CURLMsg *msg;
    while(remaining_in_queue > 0)
    {
        DEBUG_PRINT("calling curl_multi_info_read");
        msg = curl_multi_info_read(loop->multi, &remaining_in_queue);
        if(msg == NULL) {
            break;
        }
        curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (void **)&rd);
        curl_multi_remove_handle(loop->multi, rd->curl);
        rd->result = msg->data.result;
        curl_slist_free_all(rd->headers);
        rd->headers = NULL;
        free(rd->req_data_buf);
        rd->req_data_buf = NULL;
        rd->req_data_len = 0;

        DEBUG_PRINT("writing to req_out_write");
        REQUEST_TRACE_PRINT("response_complete", rd);
        write(loop->req_out_write, &rd, sizeof(AcRequestData *));
    }
    EXIT();
}


void socket_action_and_response_complete(EventLoop *loop, curl_socket_t socket, int ev_bitmask)
{
    ENTER();
    DEBUG_PRINT("loop=%p socket=%d ev_bitmask=%d", loop, socket, ev_bitmask);
    int running_handles;
    curl_multi_socket_action(loop->multi, socket, ev_bitmask, &running_handles);
    DEBUG_PRINT("after running_handlesr=%d", running_handles);
    response_complete(loop);
    EXIT();
}

/* See docs for CURLOPT_HEADERFUNCTION */

static size_t header_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    ENTER();
    AcRequestData *rd = (AcRequestData *)userdata;
    struct BufferNode *node = (struct BufferNode *)malloc(sizeof(struct BufferNode));
    node->len = size * nmemb;
    node->buffer = (char*)malloc(node->len);
    memcpy(node->buffer, ptr, node->len);
    node->next = NULL;
    if(unlikely(rd->header_buffer_head == NULL)) {
        rd->header_buffer_head = node;
    }
    if(likely(rd->header_buffer_tail != NULL)) {
        rd->header_buffer_tail->next = node;
    }
    rd->header_buffer_tail = node;
    EXIT();
    return node->len;
}

/* See docs for CURLOPT_WTIE_FUNCTION */

size_t body_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    ENTER();
    AcRequestData *rd = (AcRequestData *)userdata;
    struct BufferNode *node = (struct BufferNode *)malloc(sizeof(struct BufferNode));
    node->len = size * nmemb;
    node->buffer = (char*)malloc(node->len);
    memcpy(node->buffer, ptr, node->len);
    node->next = NULL;
    if(unlikely(rd->body_buffer_head == NULL)) {
        rd->body_buffer_head = node;
    }
    if(likely(rd->body_buffer_tail != NULL)) {
        rd->body_buffer_tail->next = node;
    }
    rd->body_buffer_tail = node;
    EXIT();
    return node->len;
}


void start_request(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask)
{
    ENTER();
    AcRequestData *rd;
    EventLoop *loop = (EventLoop*)clientData;
    int b_read = read(loop->req_in_read, &rd, sizeof(AcRequestData *));
    REQUEST_TRACE_PRINT("start_request", rd);
    DEBUG_PRINT("read AcRequestData");
    rd->curl = curl_easy_init();
    curl_easy_setopt(rd->curl, CURLOPT_SHARE, rd->session->shared);
    curl_easy_setopt(rd->curl, CURLOPT_URL, rd->url);
    curl_easy_setopt(rd->curl, CURLOPT_CUSTOMREQUEST, rd->method);
    //curl_easy_setopt(rd->curl, CURLOPT_VERBOSE, 1L); //DEBUG
    curl_easy_setopt(rd->curl, CURLOPT_ENCODING, "");
    if(rd->headers != NULL) {
        curl_easy_setopt(rd->curl, CURLOPT_HTTPHEADER, rd->headers);
    }
    if(rd->auth != NULL) {
        curl_easy_setopt(rd->curl, CURLOPT_USERPWD, rd->auth);
    }
    for(int i=0; i < rd->cookies_len; i++) {
        DEBUG_PRINT("set cookie [%s]", rd->cookies_str[i]);
        curl_easy_setopt(rd->curl, CURLOPT_COOKIELIST, rd->cookies_str[i]);
    }
    if(rd->req_data_buf != NULL) {
        curl_easy_setopt(rd->curl, CURLOPT_POSTFIELDSIZE, rd->req_data_len);
        curl_easy_setopt(rd->curl, CURLOPT_POSTFIELDS, (char*)rd->req_data_buf);
    }
    curl_easy_setopt(rd->curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(rd->curl, CURLOPT_SSL_VERIFYHOST, 0L);
    if ((rd->ca_key != NULL) && (rd->ca_cert != NULL)) {
	curl_easy_setopt(rd->curl, CURLOPT_SSLKEY, rd->ca_key);
        curl_easy_setopt(rd->curl, CURLOPT_SSLCERT, rd->ca_cert);
    }
    curl_easy_setopt(rd->curl, CURLOPT_PRIVATE, rd);
    curl_easy_setopt(rd->curl, CURLOPT_WRITEFUNCTION, body_callback);
    curl_easy_setopt(rd->curl, CURLOPT_WRITEDATA, rd);
    curl_easy_setopt(rd->curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(rd->curl, CURLOPT_HEADERDATA, rd);
    free(rd->method);
    rd->method = NULL;
    free(rd->url);
    rd->url = NULL;
    if(rd->auth != NULL) {
        free(rd->auth);
        rd->auth = NULL;
    }
    if(rd->ca_cert != NULL) {
	free(rd->ca_cert);
	rd->ca_cert = NULL;
    }
    if(rd->ca_key != NULL) {
        free(rd->ca_key);
        rd->ca_key = NULL;
    }
    free(rd->cookies_str);
    if(rd->dummy) {
        rd->result = CURLE_OK;
        curl_slist_free_all(rd->headers);
        free(rd->req_data_buf);
        write(loop->req_out_write, &rd, sizeof(AcRequestData *));
    }
    else {
        DEBUG_PRINT("adding handle");
        curl_multi_add_handle(loop->multi, rd->curl);
    }
    EXIT();
}


void stop_eventloop(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask)
{
    ENTER();
    char buffer[1];
    EventLoop *loop = (EventLoop*)clientData;
    read(loop->stop_read, buffer, sizeof(buffer));
    loop->stop = true;
    EXIT();
}

/* Used to cleanup curl handles by putting the handle back on a pipe to be picked up and cleaned up */

void curl_easy_cleanup_in_eventloop(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask)
{
    ENTER();
    CURL *curl;
    while(true) {
        int b_read = read(fd, &curl, sizeof(CURL *));
        if(b_read == -1) {
            break;
        }
        DEBUG_PRINT("curl=%p", curl);
        curl_easy_cleanup(curl);
    }
    EXIT();
}


void socket_event(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask)
{
    ENTER();
    DEBUG_PRINT("eventloop=%p fd=%d clientData=%p mask=%d (readable=%d writable=%d)", eventLoop, fd, clientData, mask, mask & AE_READABLE, mask & AE_WRITABLE);
    int ev_bitmask = 0;
    if(mask & AE_READABLE)
    {
        ev_bitmask |= CURL_CSELECT_IN;
    }
    if(mask & AE_WRITABLE)
    {
         ev_bitmask |= CURL_CSELECT_OUT;
    }
    socket_action_and_response_complete((EventLoop*)clientData, (curl_socket_t)fd, ev_bitmask);
    EXIT();
}


int socket_callback(CURL *easy, curl_socket_t s, int what, void *userp, void *socketp)
{
    ENTER();
    EventLoop *loop = (EventLoop*)userp;
    switch(what) {
        case CURL_POLL_NONE:
            DEBUG_PRINT("NONE socket=%d what=%d easy=%p", s, what, easy);
            //do nothing
            break;
        case CURL_POLL_IN:
            DEBUG_PRINT("IN socket=%d what=%d easy=%p", s, what, easy);
            aeCreateFileEvent(loop->event_loop, (int)s, AE_READABLE, socket_event, (void*)loop);
            aeDeleteFileEvent(loop->event_loop, (int)s, AE_WRITABLE);
            break;
        case CURL_POLL_OUT:
            DEBUG_PRINT("OUT socket=%d what=%d easy=%p", s, what, easy);
            aeCreateFileEvent(loop->event_loop, (int)s, AE_WRITABLE, socket_event, (void*)loop);
            aeDeleteFileEvent(loop->event_loop, (int)s, AE_READABLE);
            break;
        case CURL_POLL_INOUT:
            DEBUG_PRINT("INOUT socket=%d what=%d easy=%p", s, what, easy);
            aeCreateFileEvent(loop->event_loop, (int)s, AE_READABLE | AE_WRITABLE, socket_event, (void*)loop);
            break;
        case CURL_POLL_REMOVE:
            DEBUG_PRINT("REMOVE socket=%d what=%d easy=%p", s, what, easy);
            aeDeleteFileEvent(loop->event_loop, (int)s, AE_READABLE | AE_WRITABLE);
            break;
    };
    EXIT();
    return 0;
}


int timeout(struct aeEventLoop *eventLoop, long long id, void *clientData)
{
    ENTER();
    DEBUG_PRINT("");
    EventLoop *loop = (EventLoop*)clientData;
    loop->timer_id = NO_ACTIVE_TIMER_ID;
    socket_action_and_response_complete(loop, CURL_SOCKET_TIMEOUT, 0);
    EXIT();
    return AE_NOMORE;
}

/* See docs for CURLMOPT_TIMERFUNCTION */

int timer_callback(CURLM *multi, long timeout_ms, void *userp)
{
    ENTER();
    DEBUG_PRINT("timeout_ms=%ld", timeout_ms);
    EventLoop *loop = (EventLoop*)userp;
    if(loop->timer_id != NO_ACTIVE_TIMER_ID) {
        DEBUG_PRINT("DELETE timer_id=%ld", loop->timer_id);
        aeDeleteTimeEvent(loop->event_loop, loop->timer_id);
        loop->timer_id = NO_ACTIVE_TIMER_ID;
    }
    if(timeout_ms >= 0) {
        if((loop->timer_id = aeCreateTimeEvent(loop->event_loop, timeout_ms, timeout, userp, NULL)) == AE_ERR) {
            fprintf(stderr, "timer_callback failed\n");
            exit(1);
        }
        DEBUG_PRINT("CREATE timer_id=%ld", loop->timer_id);
    }
    EXIT();
    return 0;
}

void set_none_blocking(int fd) {
    if(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK))
    {
        fprintf(stderr, "Failed to set O_NONBLOCK on fd %d\n", fd);
        exit(1);
    }
}


static PyObject *
EventLoop_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    ENTER();
    EventLoop *self = (EventLoop *)type->tp_alloc(type, 0);
    int req_in[2];
    int req_out[2];
    int stop[2];
    int curl_easy_cleanup[2];
    self->timer_id = NO_ACTIVE_TIMER_ID;
    self->multi = curl_multi_init();
    curl_multi_setopt(self->multi, CURLMOPT_MAXCONNECTS, 1000);
    curl_multi_setopt(self->multi, CURLMOPT_SOCKETFUNCTION, socket_callback);
    curl_multi_setopt(self->multi, CURLMOPT_SOCKETDATA, self);
    curl_multi_setopt(self->multi, CURLMOPT_TIMERFUNCTION, timer_callback);
    curl_multi_setopt(self->multi, CURLMOPT_TIMERDATA, self);
    if (self != NULL) {
        self->event_loop = aeCreateEventLoop(200);
        pipe(req_in);
        self->req_in_read = req_in[0];
        set_none_blocking(self->req_in_read);
        self->req_in_write = req_in[1];
        pipe(req_out);
        self->req_out_read = req_out[0];
        set_none_blocking(self->req_out_read);
        self->req_out_write = req_out[1];
        pipe(stop);
        self->stop_read = stop[0];
        self->stop_write = stop[1];
        pipe(curl_easy_cleanup);
        self->curl_easy_cleanup_read = curl_easy_cleanup[0];
        set_none_blocking(self->curl_easy_cleanup_read);
        self->curl_easy_cleanup_write = curl_easy_cleanup[1];
        if(aeCreateFileEvent(self->event_loop, self->req_in_read, AE_READABLE, start_request, self) == AE_ERR) {
            exit(1);
        }
        if(aeCreateFileEvent(self->event_loop, self->stop_read, AE_READABLE, stop_eventloop, self) == AE_ERR) {
            exit(1);
        }
        if(aeCreateFileEvent(self->event_loop, self->curl_easy_cleanup_read, AE_READABLE, curl_easy_cleanup_in_eventloop, NULL) == AE_ERR) {
            exit(1);
        }
    }
    EXIT();
    return (PyObject *)self;
}


static void
EventLoop_dealloc(EventLoop *self)
{
    ENTER();
    DEBUG_PRINT("response=%p", self);
    curl_multi_cleanup(self->multi);
    aeDeleteEventLoop(self->event_loop);
    close(self->req_in_read);
    close(self->req_in_write);
    close(self->req_out_read);
    close(self->req_out_write);
    close(self->stop_read);
    close(self->stop_write);
    close(self->curl_easy_cleanup_read);
    close(self->curl_easy_cleanup_write);
    Py_TYPE(self)->tp_free((PyObject*)self);
}


static PyObject *
EventLoop_once(EventLoop *self, PyObject *args)
{
    ENTER();
    aeProcessEvents(self->event_loop, AE_ALL_EVENTS|AE_DONT_WAIT);
    Py_INCREF(Py_None);
    EXIT();
    return Py_None;
}


static PyObject *
EventLoop_main(EventLoop *self, PyObject *args)
{
    ENTER();
    DEBUG_PRINT("Started");
    self->thread_state = PyEval_SaveThread();
    do {
        DEBUG_PRINT("Start of aeProcessEvents");
        aeProcessEvents(self->event_loop, AE_ALL_EVENTS);
        DEBUG_PRINT("End of aeProcessEvents");
    } while(!self->stop);
    PyEval_RestoreThread(self->thread_state);
    DEBUG_PRINT("Ended");
    Py_INCREF(Py_None);
    EXIT();
    return Py_None;
}


static PyObject *
EventLoop_stop(PyObject *self, PyObject *args)
{
    ENTER();
    write(((EventLoop*)self)->stop_write, '\0', 1);
    Py_INCREF(Py_None);
    EXIT();
    return Py_None;
}

/* Get the out pipe */

static PyObject *
Eventloop_get_out_fd(PyObject *self, PyObject *args)
{
    ENTER();
    DEBUG_PRINT("");
    PyObject *rtn = Py_BuildValue("i", ((EventLoop*)self)->req_out_read);
    EXIT();
    return rtn;
}


static PyObject *
Eventloop_get_completed(PyObject *self, PyObject *args)
{
    ENTER();
    AcRequestData *rd;
    PyObject *list = PyList_New(0);
    while(true) {
        int b_read = read(((EventLoop*)self)->req_out_read, &rd, sizeof(AcRequestData *));
        if(b_read == -1) {
            break;
        }
        REQUEST_TRACE_PRINT("Eventloop_get_completed", rd);
        DEBUG_PRINT("read AcRequestData; address=%p", rd);
        PyObject *tuple = PyTuple_New(3);
        if(rd->result == CURLE_OK) {
            Response *response = PyObject_New(Response, (PyTypeObject *)&ResponseType);
            response->header_buffer = rd->header_buffer_head;
            response->body_buffer = rd->body_buffer_head;
            response->curl = rd->curl;
            response->session = rd->session;

            Py_INCREF(Py_None);
            PyTuple_SET_ITEM(tuple, 0, Py_None);
            PyTuple_SET_ITEM(tuple, 1, (PyObject*)response);
            PyTuple_SET_ITEM(tuple, 2, rd->future);
        }
        else {
            PyObject* error = PyUnicode_FromString(curl_easy_strerror(rd->result));
            free_buffer_nodes(rd->header_buffer_head);
            free_buffer_nodes(rd->body_buffer_head);
            curl_easy_cleanup(rd->curl);
            Py_DECREF(rd->session);

            PyTuple_SET_ITEM(tuple, 0, error);
            Py_INCREF(Py_None);
            PyTuple_SET_ITEM(tuple, 1, Py_None);
            PyTuple_SET_ITEM(tuple, 2, rd->future);
        }
        PyList_Append(list, tuple);
        Py_DECREF(tuple);
        if(rd->req_data_buf != NULL) {
            free(rd->req_data_buf);
        }
        Py_XDECREF(rd->cookies);
        free(rd);
    }
    EXIT();
    return list;
}


static PyMethodDef EventLoop_methods[] = {
    {"main", (PyCFunction)EventLoop_main, METH_NOARGS, "Run the event loop"},
    {"once", (PyCFunction)EventLoop_once, METH_NOARGS, "Run the event loop once"},
    {"stop", EventLoop_stop, METH_NOARGS, "Stop the event loop"},
    {"get_out_fd", Eventloop_get_out_fd, METH_NOARGS, "Get the outbound file dscriptor"},
    {"get_completed", Eventloop_get_completed, METH_NOARGS, "Get the user_object, response and error"},
    {NULL, NULL, 0, NULL}
};


static PyMemberDef EventLoop_members[] = {
    {NULL}
};


static PyTypeObject EventLoopType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "acurl.EventLoop",           /* tp_name */
    sizeof(EventLoop),           /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)EventLoop_dealloc,           /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "Event Loop Type",         /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    EventLoop_methods,         /* tp_methods */
    EventLoop_members,         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    EventLoop_new,             /* tp_new */
};



static PyObject *
Session_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    ENTER();
    Session *self;
    EventLoop *loop;

    static char *kwlist[] = {"loop", NULL};
    if (! PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist, &loop)) {
        EXIT();
        return NULL;
    }

    self = (Session *)type->tp_alloc(type, 0);
    if (self == NULL) {
        EXIT();
        return NULL;
    }

    Py_INCREF(loop);
    self->loop = loop;
    self->shared = curl_share_init();
    curl_share_setopt(self->shared, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(self->shared, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(self->shared, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    EXIT();
    return (PyObject *)self;
}


static void
Session_dealloc(Session *self)
{
    ENTER();
    DEBUG_PRINT("response=%p", self);
    curl_share_cleanup(self->shared);
    Py_XDECREF(self->loop);
    Py_TYPE(self)->tp_free((PyObject*)self);
    EXIT();
}


static PyObject *
Session_request(Session *self, PyObject *args, PyObject *kwds)
{
    ENTER();
    char *method;
    char *url;
    PyObject *future;
    PyObject *headers;
    PyObject *auth;
    PyObject *cert;
    PyObject *cookies;
    int req_data_len = 0;
    char *req_data_buf = NULL;
    int dummy;

    static char *kwlist[] = {"future", "method", "url", "headers", "auth", "cookies", "data", "dummy", "cert", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OssOOOz#pO", kwlist, &future, &method, &url, &headers, &auth, &cookies, &req_data_buf, &req_data_len, &dummy, &cert)) {
        EXIT();
        return NULL;
    }

    AcRequestData *rd = (AcRequestData *)malloc(sizeof(AcRequestData));
    REQUEST_TRACE_PRINT("Session_request", rd);
    memset(rd, 0, sizeof(AcRequestData));
    if(headers != Py_None) {
        if(!PyTuple_CheckExact(headers)) {
            PyErr_SetString(PyExc_ValueError, "headers should be a tuple of strings or None");
            goto error_cleanup;
        }
        for(int i=0; i < PyTuple_GET_SIZE(headers); i++) {
            if(!PyUnicode_CheckExact(PyTuple_GET_ITEM(headers, i))) {
                PyErr_SetString(PyExc_ValueError, "headers should be a tuple of strings or None");
                goto error_cleanup;
            }
            rd->headers = curl_slist_append(rd->headers, PyUnicode_AsUTF8(PyTuple_GET_ITEM(headers, i)));
        }
    }
    if(auth != Py_None) {
        if(!PyTuple_CheckExact(auth) || PyTuple_GET_SIZE(auth) != 2 || !PyUnicode_CheckExact(PyTuple_GET_ITEM(auth, 0)) || !PyUnicode_CheckExact(PyTuple_GET_ITEM(auth, 1))) {
	    PyErr_SetString(PyExc_ValueError, "auth should be a tuple of strings (username, password) or None");
            goto error_cleanup;
        }
        char *username = PyUnicode_AsUTF8(PyTuple_GET_ITEM(auth, 0));
        char *password = PyUnicode_AsUTF8(PyTuple_GET_ITEM(auth, 1));
        rd->auth = (char*)malloc(strlen(username) + 1 + strlen(password) + 1);
        sprintf(rd->auth, "%s:%s", username, password);
    }
    if(cert != Py_None) {
	if(!PyTuple_CheckExact(cert) || PyTuple_GET_SIZE(cert) != 2 || !PyUnicode_CheckExact(PyTuple_GET_ITEM(cert, 0)) || !PyUnicode_CheckExact(PyTuple_GET_ITEM(cert, 1))) {
            PyErr_SetString(PyExc_ValueError, "cert should be a tuple of strings (certificate path, key path) or None");
            goto error_cleanup;
        }
        DEBUG_PRINT("PRE variables");
        char *cert_path = PyUnicode_AsUTF8(PyTuple_GET_ITEM(cert, 0));
        char *key_path = PyUnicode_AsUTF8(PyTuple_GET_ITEM(cert, 1));
        DEBUG_PRINT("PRE malloc");
        rd->ca_cert = (char*)malloc(strlen(cert_path) + 1);
        rd->ca_key = (char*)malloc(strlen(key_path) + 1);
        DEBUG_PRINT("PRE sprintf");
        sprintf(rd->ca_cert, "%s", cert_path);
        sprintf(rd->ca_key, "%s", key_path);
    }
    if(cookies != Py_None) {
        Py_INCREF(cookies);
        rd->cookies = cookies;
        if(!PyTuple_CheckExact(cookies)) {
            PyErr_SetString(PyExc_ValueError, "cookies should be a tuple of strings or None");
            goto error_cleanup;
        }
        rd->cookies_len = PyTuple_GET_SIZE(cookies);
        if(rd->cookies_len > 0) {
            rd->cookies_str = (char**)calloc(PyTuple_GET_SIZE(cookies), sizeof(char*));
            for(int i=0; i < PyTuple_GET_SIZE(cookies); i++) {
                if(!PyUnicode_CheckExact(PyTuple_GET_ITEM(cookies, i))) {
                    PyErr_SetString(PyExc_ValueError, "cookies should be a tuple of strings or None");
                    goto error_cleanup;
                }
                rd->cookies_str[i] = PyUnicode_AsUTF8(PyTuple_GET_ITEM(cookies, i));
            }
        }
    }
    Py_INCREF(self);
    rd->session = self;
    Py_INCREF(future);
    rd->future = future;
    rd->method = strdup(method);
    rd->url = strdup(url);
    if(req_data_buf != NULL) {
        req_data_buf = strdup(req_data_buf);
    }
    rd->req_data_len = req_data_len;
    rd->req_data_buf = req_data_buf;
    rd->dummy = dummy;
    write(self->loop->req_in_write, &rd, sizeof(AcRequestData *));
    DEBUG_PRINT("scheduling request");
    Py_INCREF(Py_None);
    EXIT();
    return Py_None;

    error_cleanup:
    if(rd->headers) {
        curl_slist_free_all(rd->headers);
    }
    if(rd->auth) {
        free(rd->auth);
    }
    if(rd->ca_cert) {
	free(rd->ca_cert);
    }
    if(rd->ca_key) {
        free(rd->ca_key);
    }
    if(rd->cookies) {
        Py_DECREF(rd->cookies);
        free(rd->cookies_str);
    }
    free(rd);
    EXIT();
    return NULL;
}


static PyMethodDef Session_methods[] = {
    {"request", (PyCFunction)Session_request, METH_VARARGS | METH_KEYWORDS, "Send a request"},
    {NULL, NULL, 0, NULL}
};


static PyTypeObject SessionType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "acurl.Session",           /* tp_name */
    sizeof(Session),           /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)Session_dealloc,           /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "Session Type",            /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    Session_methods,           /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    Session_new,               /* tp_new */
};


const static char MODULE_NAME[] = "_acurl";


static PyMethodDef module_methods[] = {
    {NULL, NULL, 0, NULL}
};


static struct PyModuleDef _acurl_module = {
   PyModuleDef_HEAD_INIT,
   MODULE_NAME,
   NULL,
   -1,
   module_methods
};


PyMODINIT_FUNC
PyInit__acurl(void)
{
    PyObject* m;

    if (PyType_Ready(&SessionType) < 0)
        return NULL;

    if (PyType_Ready(&EventLoopType) < 0)
        return NULL;

    if (PyType_Ready(&ResponseType) < 0)
        return NULL;

    m = PyModule_Create(&_acurl_module);

    if(m != NULL) {
        curl_global_init(CURL_GLOBAL_ALL); // init curl library
        Py_INCREF(&SessionType);
        PyModule_AddObject(m, "Session", (PyObject *)&SessionType);
        Py_INCREF(&EventLoopType);
        PyModule_AddObject(m, "EventLoop", (PyObject *)&EventLoopType);
        Py_INCREF(&ResponseType);
        PyModule_AddObject(m, "Response", (PyObject *)&ResponseType);
    }

    return m;
}
