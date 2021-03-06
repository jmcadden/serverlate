#include <netinet/tcp.h>

#include <locale> 

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/thread.h>
#include <event2/util.h>

#include "config.h"

#include "Connection.h"
#include "distributions.h"
#include "Generator.h"
#include "serverlate.h"
#include "util.h"

evhttp_cmd_type strToHttpReq(std::string req){
  if ( req == "get" || req == "GET")
      return EVHTTP_REQ_GET;
  if (req == "post" || req == "POST")
      return EVHTTP_REQ_POST;
  DIE("Unknown operation type: %s\n", req.c_str());
}

/**
 * Create a new connection to a server endpoint.
 */
Connection::Connection(struct event_base* _base, struct evdns_base* _evdns,
                       Json::Value operation, options_t _options,
                       bool sampling) :
  start_time(0), stats(sampling), options(_options),
  base(_base), evdns(_evdns)
{
  // Extract operation values from json structure operation
  auto h = operation.get("hostname", "localhost").asString();
  type = operation.get("method", "get").asString();
  hostname = string(evhttp_uri_get_host(evhttp_uri_parse(h.c_str())));
  V("DEBUG HOSTNAME: %s\n", hostname.c_str());
	port = operation.get("port", "80").asString();
  uri = operation.get("path", "/").asString();
  // headers
  if(operation.isMember("headers")){
    for( auto f : operation["headers"].getMemberNames()){
        auto c = operation["headers"][f];
        headers[f] = c.asString();
    }
  }
  I("%s\n", print_operation().c_str());

  valuesize = createGenerator(options.valuesize);
  keysize = createGenerator(options.keysize);
  keygen = new KeyGenerator(keysize, options.records);

  if (options.lambda <= 0) {
    iagen = createGenerator("0");
  } else {
    D("iagen = createGenerator(%s)", options.ia);
    iagen = createGenerator(options.ia);
    iagen->set_lambda(options.lambda);
  }

  read_state  = IDLE;
  write_state = INIT_WRITE;

  last_tx = last_rx = 0.0;

	// For simplicity, we let DNS resolution block. Everything else should be
	// asynchronous though.
	evcon = evhttp_connection_base_new(base, evdns, hostname.c_str(), atoi(port.c_str()));
  timer = evtimer_new(base, timer_cb, this);
}

/**
 * Destroy a connection, performing cleanup.
 */
Connection::~Connection() {
  event_free(timer);
  timer = NULL;
  // FIXME:  W("Drain op_q?");
  //bufferevent_free(bev);

  delete iagen;
  delete keygen;
  delete keysize;
  delete valuesize;
}

/**
 * Reset the connection back to an initial, fresh state.
 */
void Connection::reset() {
  // FIXME: Actually check the connection, drain all bufferevents, drain op_q.
  assert(op_queue.size() == 0);
  evtimer_del(timer);
  read_state = IDLE;
  write_state = INIT_WRITE;
  stats = ConnectionStats(stats.sampling);
}

/**
 * Set our event processing priority.
 */
void Connection::set_priority(int pri) { DIE("UNIMPLEMENTED"); }

/**
 * Load any required test data onto the server.
 */
void Connection::start_loading() { DIE("UNIMPLEMENTED"); }

/**
 * http request callback
 */
void Connection::request_callback(struct evhttp_request *req){

	if (!req) {
		/* If req is NULL, it means an error occurred */
    DIE("Uh oh.. Request returned is NULL ");
	}

  if (op_queue.size() == 0) DIE("Spurious read callback.");
  Operation *op = &op_queue.front();;

  auto ret = evhttp_request_get_response_code(req);
  switch(ret){
    case 0: // Connection refused
      DIE("Failed to connect to server: Connection refused\n");
    case HTTP_OK:
    case 202: // HTTP_ACCEPTED
      break;
    case HTTP_NOCONTENT:
    case HTTP_MOVEPERM:
    case HTTP_MOVETEMP:
    case HTTP_NOTMODIFIED:
    case HTTP_BADREQUEST:
    case HTTP_NOTFOUND:
    case HTTP_BADMETHOD:
    case HTTP_ENTITYTOOLARGE:
    case HTTP_EXPECTATIONFAILED:
    case HTTP_INTERNAL:
    case HTTP_NOTIMPLEMENTED:
    case HTTP_SERVUNAVAIL:
      stats.get_misses++;
	    D("Warning: Received %d response\n", evhttp_request_get_response_code(req));
      break;
    default: 
	    DIE("Error: Unknown response code: %d\n", evhttp_request_get_response_code(req));
  }
  finish_op(op);
      
  char buffer[256];
  int nread;
	while ((nread = evbuffer_remove(evhttp_request_get_input_buffer(req),
		    buffer, sizeof(buffer))) > 0) {
    stats.rx_bytes += nread;
    // dump response payload to stdout
		//fwrite(buffer, nread, 1, stdout);
	}
}

/**
 * Issue either a get or set request to the server according to our probability distribution.
 */
void Connection::issue_something(double now) {
  char key[256];
  // FIXME: generate key distribution here!
  string keystr = keygen->generate(lrand48() % options.records);
  strcpy(key, keystr.c_str());

#if 0
  // Generate a random key
  if (drand48() < options.update) {
    int index = lrand48() % (1024 * 1024);
    issue_post(key, &random_char[index], valuesize->generate(), now);
  } else {
    issue_get(key, now);
  }
#endif 

  issue_request(key, now, EVHTTP_REQ_POST);
}


void Connection::issue_request(const char* key, double now, evhttp_cmd_type type)
 {
  Operation op;

#if HAVE_CLOCK_GETTIME
  op.start_time = get_time_accurate();
#else
  if (now == 0.0) {
#if USE_CACHED_TIME
    struct timeval now_tv;
    event_base_gettimeofday_cached(base, &now_tv);
    op.start_time = tv_to_double(&now_tv);
#else
    op.start_time = get_time();
#endif
  } else {
    op.start_time = now;
  }
#endif

  op.key = string(key);
  op.type = type;
  op_queue.push(op);

  if (read_state == IDLE) read_state = WAITING_FOR_GET;

  auto req = evhttp_request_new(bev_request_cb, this);
	if (req == NULL) {
		DIE("evhttp_request_new() failed\n");
	}
	auto output_headers = evhttp_request_get_output_headers(req);
	evhttp_add_header(output_headers, "Host", hostname.c_str());
  // TODO: may not want to close connection each time
	evhttp_add_header(output_headers, "Connection", "close");

  for(  const auto &h : headers){
	  evhttp_add_header(output_headers, h.first.c_str(), h.second.c_str());
    D("Added Header: %s %s", h.first.c_str(), h.second.c_str());
  }
  if (evhttp_make_request(evcon, req, type, uri.c_str()) < 0){
    DIE("REQUEST FAILED!");
  }
 }

void Connection::issue_post(const char* key, const char* value, int length,
                           double now) {
  DIE("ISSUE POST");
}
void Connection::issue_get(const char* key, double now) {
  DIE("ISSUE GET");
}

/**
 * Return the oldest live operation in progress.
 */
void Connection::pop_op() {
  assert(op_queue.size() > 0);

  op_queue.pop();

  if (read_state == LOADING) return;
  read_state = IDLE;

  // Advance the read state machine.
  if (op_queue.size() > 0) {
    DIE("UNIMPLEMENTED");
  }
}

/**
 * Finish up (record stats) an operation that just returned from the
 * server.
 */
void Connection::finish_op(Operation *op) {
  double now;
#if USE_CACHED_TIME
  struct timeval now_tv;
  event_base_gettimeofday_cached(base, &now_tv);
  now = tv_to_double(&now_tv);
#else
  now = get_time();
#endif
#if HAVE_CLOCK_GETTIME
  op->end_time = get_time_accurate();
#else
  op->end_time = now;
#endif

  switch (op->type) {
  case EVHTTP_REQ_GET: stats.log_get(*op); break;
  case EVHTTP_REQ_POST: stats.log_post(*op); break;
  default: DIE("Not implemented.");
  }

  last_rx = now;
  pop_op();
  drive_write_machine();
}

/**
 * Check if our testing is done and we should exit.
 */
bool Connection::check_exit_condition(double now) {
  if (read_state == INIT_READ) return false;
  if (now == 0.0) now = get_time();
  if (now > start_time + options.time) return true;
  if (options.loadonly && read_state == IDLE) return true;
  return false;
}

/**
 * Handle new connection and error events.
 */
void Connection::event_callback(short events) {
    DIE("THIS HAPPENED...");
  if (events & BEV_EVENT_CONNECTED) {
    D("Connected to %s:%s.", hostname.c_str(), port.c_str());
    int fd = 0;//bufferevent_getfd(bev);
    if (fd < 0) DIE("bufferevent_getfd");

    if (!options.no_nodelay) {
      int one = 1;
      if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     (void *) &one, sizeof(one)) < 0)
        DIE("setsockopt()");
    }

    read_state = CONN_SETUP;
    if (prot->setup_connection_w()) {
      read_state = IDLE;
    }

  } else if (events & BEV_EVENT_ERROR) {
    int err = 0;//bufferevent_socket_get_dns_error(bev);
    if (err) DIE("DNS error: %s", evutil_gai_strerror(err));
    DIE("BEV_EVENT_ERROR: %s", strerror(errno));

  } else if (events & BEV_EVENT_EOF) {
    DIE("Unexpected EOF from server.");
  }
}

/**
 * Request generation loop. Determines whether or not to issue a new command,
 * based on timer events.
 *
 * Note that this function loops. Be wary of break vs. return.
 */
void Connection::drive_write_machine(double now) {
  if (now == 0.0) now = get_time();

  double delay;
  struct timeval tv;

  if (check_exit_condition(now)) return;

  while (1) {
    switch (write_state) {
    case INIT_WRITE:
      delay = iagen->generate();
      next_time = now + delay;
      double_to_tv(delay, &tv);
      evtimer_add(timer, &tv);
      write_state = WAITING_FOR_TIME;
      break;

    case ISSUING:
      if (op_queue.size() >= (size_t) options.depth) {
        write_state = WAITING_FOR_OPQ;
        return;
      } else if (now < next_time) {
        write_state = WAITING_FOR_TIME;
        break; // We want to run through the state machine one more time
               // to make sure the timer is armed.
      } else if (options.moderate && now < last_rx + 0.00025) {
        write_state = WAITING_FOR_TIME;
        if (!event_pending(timer, EV_TIMEOUT, NULL)) {
          delay = last_rx + 0.00025 - now;
          double_to_tv(delay, &tv);
          evtimer_add(timer, &tv);
        }
        return;
      }

      issue_something(now);
      last_tx = now;
      stats.log_op(op_queue.size());
      next_time += iagen->generate();

      if (options.skip && options.lambda > 0.0 &&
          now - next_time > 0.005000 &&
          op_queue.size() >= (size_t) options.depth) {

        while (next_time < now - 0.004000) {
          stats.skips++;
          next_time += iagen->generate();
        }
      }
      break;

    case WAITING_FOR_TIME:
      if (now < next_time) {
        if (!event_pending(timer, EV_TIMEOUT, NULL)) {
          delay = next_time - now;
          double_to_tv(delay, &tv);
          evtimer_add(timer, &tv);
        }
        return;
      }
      write_state = ISSUING;
      break;

    case WAITING_FOR_OPQ:
      if (op_queue.size() >= (size_t) options.depth) return;
      write_state = ISSUING;
      break;

    default: DIE("Not implemented");
    }
  }
}

/**
 * Handle incoming data (responses).
 */
void Connection::read_callback() {
  struct evbuffer *input = NULL;//bufferevent_get_input(bev);

  Operation *op = NULL;
  bool done, full_read;

  if (op_queue.size() == 0) V("Spurious read callback.");

  while (1) {
    if (op_queue.size() > 0) op = &op_queue.front();

    switch (read_state) {
    case INIT_READ: DIE("event from uninitialized connection");
    case IDLE: return;  // We munched all the data we expected?

    case WAITING_FOR_GET:
      assert(op_queue.size() > 0);
      full_read = prot->handle_response(input, done);
      if (!full_read) {
        return;
      } else if (done) {
        finish_op(op); // sets read_state = IDLE
      }
      break;

    case WAITING_FOR_POST:
      assert(op_queue.size() > 0);
      if (!prot->handle_response(input, done)) return;
      finish_op(op);
      break;

    case LOADING:
      assert(op_queue.size() > 0);
      if (!prot->handle_response(input, done)) return;
      loader_completed++;
      pop_op();

      if (loader_completed == options.records) {
        D("Finished loading.");
        read_state = IDLE;
      } else {
        while (loader_issued < loader_completed + LOADER_CHUNK) {
          if (loader_issued >= options.records) break;

          char key[256];
          string keystr = keygen->generate(loader_issued);
          strcpy(key, keystr.c_str());
          int index = lrand48() % (1024 * 1024);
          issue_post(key, &random_char[index], valuesize->generate());

          loader_issued++;
        }
      }

      break;

    case CONN_SETUP:
      assert(false);//assert(options.binary);
      if (!prot->setup_connection_r(input)) return;
      read_state = IDLE;
      break;

    default: DIE("not implemented");
    }
  }
}

/**
 * Callback called when write requests finish.
 */
void Connection::write_callback() {}

/**
 * Callback for timer timeouts.
 */
void Connection::timer_callback() { drive_write_machine(); }


/* The follow are C trampolines for libevent callbacks. */
void bev_event_cb(struct bufferevent *bev, short events, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->event_callback(events);
}

void bev_read_cb(struct bufferevent *bev, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->read_callback();
}

void bev_write_cb(struct bufferevent *bev, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->write_callback();
}

void timer_cb(evutil_socket_t fd, short what, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->timer_callback();
}

void bev_request_cb(struct evhttp_request *req, void *ptr){
  Connection* conn = (Connection*) ptr;
  conn->request_callback(req);
}
