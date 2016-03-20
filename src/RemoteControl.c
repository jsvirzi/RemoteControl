#include <RemoteControl.h>

#include <stdlib.h>

static void *server_loop(void *ext) {

	int reply_buffer_length = params->reply_buffer_length;
	int log_buff_length = params->log_buff_length;

	int server_version;
	int socket_keep_alive; /* keep a socket alive for 5 seconds after last activity */
/* these two must keep lockstep. jsv make into structures */
	int *socket_index;
	int *socket_sunset;
	int n_sockets;
	char *reply_buffer = new char [ reply_buffer_length ];
	struct sockaddr_in cli_addr;
	struct sockaddr_in srv_addr;
	int n, sockfd, port; 
	socklen_t clilen;
	bool status;
/* easier to keep track */
	struct pollfd poll_fd;
	std::vector<struct pollfd> poll_fds;
/* this is what the ioctl call wants to see */
#define POLLSIZE 32
	struct pollfd poll_set[POLLSIZE];
	int num_fds;

	RemoteControl::ServerLoopParams *params = (RemoteControl::ServerLoopParams *)ext;

	char *log_buff = params->log_buff;

	snprintf(log_buff, log_buff_length, "starting server service");
	log_fxn(RemoteControl::LOG_LEVEL_INFO, log_buff);

	RemoteControl::CallbackExt server_callback_ext;

	*params->run = 1;
	*params->thread_running = 1;

	while(*params->run) {

		usleep(sleep_wait); /* loop pacer */

		int i, n_read, curtime = time(0);

		if(n_sockets > 0) printf("%d sockets open\n", n_sockets);

for(i=0;i<n_sockets;++i) printf("socket_index(%d) = %d\n", i, socket_index[i]);
		num_fds = poll_fds.size();
for(i=0;i<num_fds;++i) printf("poll socket(%d) = %d\n", i, poll_fds.at(i).fd);

		/* the first one is always *sockfd* */

		num_fds = 0;

		memset(&poll_set[num_fds], 0, sizeof(pollfd));
		poll_set[num_fds].fd = sockfd;
		poll_set[num_fds].events = POLLIN;
		++num_fds;
		
		std::vector<struct pollfd>::const_iterator it, last = poll_fds.end();
		for(it = poll_fds.begin();it != last;++it) {
			memset(&poll_set[num_fds], 0, sizeof(pollfd));
			poll_set[num_fds].fd = it->fd;
			poll_set[num_fds].events = it->events;
			++num_fds;
		}

		poll(poll_set, num_fds, 1000);

		poll_fds.clear();

		for(i=1;i<num_fds;++i) {

			int fd = poll_set[i].fd;

		/* check for unused sockets, e.g. - those that have gone some time w/o activity */
			int sunset = get_socket_sunset(fd);
			if((0 < sunset) && (sunset < curtime)) {
				unmap_socket(fd); /* closes fd */
printf("socket %d timeout. closed\n", fd);
				continue;
			} else if(sunset < 0) {
printf("error: socket %d multiply mapped\n", fd); /* jsv. not sure about what action to take */
			}

		/* nothing to do. put this back into the pool and move on */
			if((poll_set[i].revents & POLLIN) == 0) {
				poll_fd.fd = poll_set[i].fd;
				poll_fd.events = POLLIN;
				poll_fds.push_back(poll_fd);
				continue;
			}

		/* accept any incoming requests. should only happen when i = 0 */ 
			ioctl(poll_set[i].fd, FIONREAD, &n_read);
			if(n_read == 0) {
				unmap_socket(poll_set[i].fd); /* closes fd */
			} else {
			/* new timeout based on recent activity */
				register_socket_sunset(poll_set[i].fd, socket_keep_alive);
				bzero(reply_buffer, reply_buffer_length);
				n = read(poll_set[i].fd, reply_buffer, reply_buffer_length-1);
				if(n < 0) {
					snprintf(log_buff, log_buff_length, "ERROR reading from socket");
					if(log_fxn) log_fxn(RemoteControl::LOG_LEVEL_WARNING, log_buff);
				}
printf("received message: [%s]\n", reply_buffer);
				poll_fd.fd = fd;
				poll_fd.events = POLLIN;
				poll_fds.push_back(poll_fd);

				std::vector<std::string> elements;
				split(elements, reply_buffer, n, " \t\n?;");

				n = elements.size();

// for(i=0;i<n;++i) { printf("element(%d) = [%s]\n", i, elements.at(i).c_str()); }

			/* callbacks */
				std::vector<CallbackParams>::const_iterator cbiter, cblast = callback_params.end();
				for(cbiter = callback_params.begin();cbiter != cblast; ++cbiter) {
					CallbackFxn *fxn = cbiter->fxn;
					void *ext = cbiter->ext;
					(*fxn)(this, fd, elements, ext);
				}

			}
		}

		/* accept any incoming requests */
		int client_sockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
		while(client_sockfd > 0) {
if(verbose) printf("accepted connection client = %d bound to server = %d\n", client_sockfd, sockfd);
			set_nonblocking(client_sockfd);
			memset(&poll_fd, 0, sizeof(poll_fd));
			poll_fd.fd = client_sockfd;
			poll_fd.events = POLLIN;
			poll_fds.push_back(poll_fd);
			map_socket(client_sockfd);
			register_socket_sunset(client_sockfd, socket_keep_alive);
if(verbose) printf("adding client on %d (i=%d)\n", client_sockfd, poll_set[i].fd);
			client_sockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
		}

	}

	delete [] log_buff;
	delete [] reply_buff;

	*params->thread_running = 0;

}

/* parses an integer from a string in the form HEADER=1234.
	input: str is the string to parse
	input: hdr is the string corresponding to the header.
	in the example within this comment str = "HEADER=1234" and hdr = "HEADER" */
static bool parse_integer(const char *str, const char *hdr, int *result, int dflt) {
	const char *p = strstr(str, hdr);
// printf("%s %s %p\n", str, hdr, p);
	if(p == NULL) {
		if(*result) *result = dflt;
		return false;
	}
	const char *delims = "&;"; /* delimiters for terminating the string */
	int i, j, len = strlen(hdr), n_delims = strlen(delims);
	p += len + 1; /* get past the header + "=" */
	len = strlen(p); /* length of remaining substring after stripping out header */
	char *buff = new char [ len + 1 ];
	memcpy(buff, p, len + 1);
// printf("parse_integer = %s\n", buff);
	for(i=0;i<len;++i) {
		int found = 0;
		for(j=0;j<n_delims;++j) {
			if(buff[i] == delims[j]) {
				buff[i] = 0;
				found = 1;
				break;
			}
		}
		if(found) break;
	}
	i = atoi(buff);
	if(result) *result = i;
	delete [] buff;
	return true;
}

/* parses an integer from a string in the form HEADER=1234.
	input: str is the string to parse
	input: hdr is the string corresponding to the header.
	in the example within this comment str = "HEADER=1234" and hdr = "HEADER" */
static bool parse_float(const char *str, const char *hdr, float *result, float dflt) {
	const char *p = strstr(str, hdr);
// printf("%s %s %p\n", str, hdr, p);
	if(p == NULL) {
		if(*result) *result = dflt;
		return false;
	}
	const char *delims = "&;"; /* delimiters for terminating the string */
	int i, j, len = strlen(hdr), n_delims = strlen(delims);
	p += len + 1; /* get past the header + "=" */
	len = strlen(p); /* length of remaining substring after stripping out header */
	char *buff = new char [ len + 1 ];
	memcpy(buff, p, len + 1);
// printf("parse_float = %s\n", buff);
	for(i=0;i<len;++i) {
		int found = 0;
		for(j=0;j<n_delims;++j) {
			if(buff[i] == delims[j]) {
				buff[i] = 0;
				found = 1;
				break;
			}
		}
		if(found) break;
	}
	float x = atof(buff);
	if(result) *result = x;
	delete [] buff;
	return true;
}

/* parses a string from a string in the form TEXT="It was the best of times"&AUTHOR="Charles Dickens" (Note: "" are required).
	input: str is the string to parse
	input: hdr is the string corresponding to the header.
	in the example within this comment,
	str = TEXT="It was the best of times"&AUTHOR="Charles Dickens" and hdr = "TEXT".
	the function will return the string "It was the best of times" (no quotes) */
static bool parse_string(const char *str, const char *hdr, std::string *result, std::string &dflt) {
	const char *p = strstr(str, hdr);
// printf("%s %s %p\n", str, hdr, p);
	if(p == NULL) {
		if(result) *result = dflt;
		return false;
	}
	const char *delims = "&;"; /* delimiters for terminating the string */
	int i, j, len = strlen(hdr), n_delims = strlen(delims);
	p += len + 1; /* get past the header + "=" */
	len = strlen(p); /* length of remaining substring after stripping out header */
	char *buff = new char [ len + 1 ];
	memcpy(buff, p, len + 1);
// printf("parse_string = %s\n", buff);
	for(i=0;i<len;++i) {
		int found = 0;
		for(j=0;j<n_delims;++j) {
			if(buff[i] == delims[j]) {
				buff[i] = 0;
				found = 1;
				break;
			}
		}
		if(found) break;
	}
	if(result) *result = buff;
	delete [] buff;
	return i;
}

static bool is_delimiter(char ch, const char *delims) {
	int n = strlen(delims);
	for(int i=0;i<n;++i) if(ch == delims[i]) return true;
	return false;
}

static bool split(std::vector<std::string> &elements, const char *buffer, int len, const char *delims) {
	if(delims == 0) delims = " \t\n"; /* the defaults */
	if(len == 0) len = strlen(buffer);
	char *str = new char [ len + 1 ];
	int state = 0, i, j;
	for(i=0;i<len;++i) {
		char ch = buffer[i];
		if(state == 0 && !is_delimiter(ch, delims)) {
			j = 0;
			str[j++] = ch;
			state = 1;
		} else if(state == 1 && !is_delimiter(ch, delims)) {
			str[j++] = ch;
		} else if(state == 1) {
			str[j++] = 0;
			elements.push_back(std::string(str));
			state = 0;
		} 
	}
/* finalize state machine. if buffer is not null-terminated, we could be left with a straggler */
	if(state == 1) {
		str[j++] = 0;
		elements.push_back(std::string(str));
	}
	delete [] str;
	return true;
}

bool RemoteControl::register_callback(CallbackFxn *fxn, void *ext) {
	CallbackParams params;
	params.fxn = fxn;
	params.ext = ext;
	callback_params.push_back(params);
};

RemoteControl::~RemoteControl() {
	delete [] socket_index;
	delete [] socket_sunset;
}

RemoteControl::RemoteControl(int port, int max_sockets) {
	status = false;
	server_version = 1;
	socket_keep_alive = 5; /* keep a socket alive for 5 seconds after last activity */
/* these two must keep lockstep. jsv make into structures */
	socket_index = new int [ max_sockets ];
	socket_sunset = new int [ max_sockets ];
	n_sockets = 0;
	reply_buffer_length = 16384; 
	reply_buffer_length = 4 * 1280 * 960; /* huge for sending images */
	reply_buffer = new char [ reply_buffer_length ];

	this->port = port; 
	memset(&cli_addr, 0, sizeof(struct sockaddr_in));
	memset(&srv_addr, 0, sizeof(struct sockaddr_in));

	log_buff_length = 1024;
	log_buff = new char [ log_buff_length + 1 ];

	log_fxn = 0;

	clilen = sizeof(cli_addr);
 
	srv_addr.sin_family = AF_INET;
	srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	srv_addr.sin_port = htons(port);

	if((sockfd = socket(AF_INET, SOCK_STREAM,0)) < 0) {
		snprintf(log_buff, log_buff_length, "error opening socket");
		if(log_fxn) log_fxn(LOG_LEVEL_ERROR, log_buff);
		status = false;
	}

	if(bind(sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
		snprintf(log_buff, log_buff_length, "error opening socket");
		if(log_fxn) log_fxn(LOG_LEVEL_ERROR, log_buff);
		status = false;
	}

	if(listen(sockfd, 64) < 0) {
		snprintf(log_buff, log_buff_length, "error opening socket");
		if(log_fxn) log_fxn(LOG_LEVEL_ERROR, log_buff);
		status = false;
	}

	set_nonblocking(sockfd);

	num_fds = 0;
	memset(poll_set, 0, sizeof(poll_set));
}

bool RemoteControl::init(unsigned int cpu_mask) {
	const char *name = "\"RemoteControl\""; // jsv move to include

/* create a new thread for the server loop */
	server_loop_params.log_buff_length = log_buff_length;
	server_loop_params.log_buff = new char [ server_loop_params.log_buff_length ]; /* thread-safe, its own buffer */
	server_loop_params.log_fxn = log_fxn;
	server_loop_params.thread_running = &thread_running;
	server_loop_params.reply_buff_length = 1024; // jsv move to configurable 
	server_loop_params.run = &run;

	int err = pthread_create(&tid, NULL, &server_loop, (void *)server_loop_params);

	if(cpu_mask) { /* are we requesting this thread to be on a particular core? */

		snprintf(log_buff, log_buff_length, "%s configured to run on core mask = 0x8.8%x", name, cpu_mask);
		if(log_fxn) log_fxn(LOG_LEVEL_INFO, log_buff);

		cpu_set_t cpu_set;
		CPU_ZERO(&cpu_set);
		for(int i=0;i<32;++i) { if(cpu_mask & (1 << i)) CPU_SET(i, &cpu_set); }
		err = pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cpu_set);
		if(err) {
			snprintf(log_buff, log_buff_length, "unable to set thread affinity for %s", name);
			if(log_fxn) log_fxn(LOG_LEVEL_ERROR, log_buff);
		}
		err = pthread_getaffinity_np(tid, sizeof(cpu_set_t), &cpu_set);
		if(err) {
			snprintf(log_buff, log_buff_length, "unable to get thread affinity for %s", name);
			if(log_fxn) log_fxn(LOG_LEVEL_ERROR, log_buff);
		} else {
			for(int i=0;i<CPU_SETSIZE;++i) {
				if(CPU_ISSET(i, &cpu_set)) {
					snprintf(log_buff, log_buff_length, "thread affinity for %s = %d", name, i);
					if(log_fxn) log_fxn(LOG_LEVEL_ERROR, log_buff);
				}
			}
		}
	}

	/* wait for the thread to start */
	int time0 = time(NULL);
	while(thread_running == 0) {
		int dtime = time(NULL) - time0;
		if(dtime > 5) {
			snprintf(log_buff, log_buff_length, 
				"timeout waiting for %s thread to initialize (%d seconds)...", name, dtime);
			if(log_fxn) (*log_fxn)(LOG_LEVEL_ERROR, log_buff);
			return false;
		}
		snprintf(log_buff, log_buff_length, "waiting for %s thread to initialize (%d seconds)...", name, dtime);
		if(log_fxn) (*log_fxn)(LOG_LEVEL_INFO, log_buff);
		usleep(1000000);
	}
	snprintf(log_buff, log_buff_length, "%s thread successfully initialized", name);
	if(log_fxn) log_fxn(LOG_LEVEL_INFO, log_buff);

	return true;
}

#if 0
bool wait_for_thread_to_stop(pthread_t *thread, int *flag, const char *name, int timeout, LogInfo *log) {
	char *thread_result;
	while(*flag) {
		if(time(0) > timeout) {
			snprintf(log->buff, log->buff_length, "CAMERA: %s thread termination timeout", name);
			if(log->fxn) log->fxn(LOG_LEVEL_FATAL, log->buff);
			return false;
		}
		snprintf(log->buff, log->buff_length, "CAMERA: waiting for %s thread to terminate", name);
		if(log->fxn) log->fxn(LOG_LEVEL_INFO, log->buff);
		usleep(100000);
	}

/* and then check the thread function actually returned */
	snprintf(log->buff, log->buff_length, "shutting down %s service", name);
	log->fxn(LOG_LEVEL_INFO, log->buff);
	pthread_join(*thread, (void**) &thread_result);
	snprintf(log->buff, log->buff_length, "CAMERA: %s service stopped", name);
	log->fxn(LOG_LEVEL_INFO, log->buff);

	return true;
}
#endif

int RemoteControl::initialize_socket_map() {
	int i;
	for(int i=0;i<MAXSOCKETS;++i) socket_index[i] = socket_sunset[i] = 0; 
	n_sockets = 0;
	return 0;
}

int RemoteControl::map_socket(int fd) {
printf("map_socket(%d)\n", fd);
	int i;
/* make sure this socket isn't already being used */
	for(i=0;i<n_sockets;++i) {
		if(socket_index[i] == fd) return -fd;
	}
	int rc = n_sockets;
	socket_index[n_sockets] = fd;
	socket_sunset[n_sockets] = 0;
	++n_sockets;
	return rc;
}

/* it shouldn't happen, but if it does, 
   unmap_socket() will clean out all instances of the socket fd */
int RemoteControl::unmap_socket(int fd) {
printf("unmap_socket(%d)\n", fd);
	if(fd <= 0) return -1;
	int i, j, count = 1;
	for(i=0;i<n_sockets;++i) {
		if(socket_index[i] == fd) {
			++count;
			if(count == 1) ::close(fd);
			for(j=i+1;j<n_sockets;++j) {
				socket_index[j-1] = socket_index[j];
				socket_sunset[j-1] = socket_sunset[j];
			}
			--n_sockets;
		} 
	}
	return (count == 1) ? 0 : -1;
}

int RemoteControl::get_socket_sunset(int fd) {
// printf("get_socket_sunset(%d) n_sockets = %d\n", fd, n_sockets);
	int i, sunset = 0, count = 0;
	for(i=0;i<n_sockets;++i) {
// printf("get_socket_sunset(): socket_index[%d] = %d\n", i, socket_index[i]);
		if(socket_index[i] == fd) {
			sunset = socket_sunset[i];
			++count;
		} 
	}
	if(count > 1) { printf("ERROR: socket multiple entries\n"); }
	return (count > 1) ? -1 : sunset;
}

int RemoteControl::register_socket_sunset(int fd, int timeout) {
	int i, curtime = time(0), count = 0;
	for(i=0;i<n_sockets;++i) {
		if(socket_index[i] == fd) {
			socket_sunset[i] = curtime + timeout;
			++count;
		}
	}
	return (count > 1) ? -1 : 0;
}

int RemoteControl::set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if(flags == -1) flags = 0;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* nbytes indicates how many bytes will be sent immediately after this call */
bool RemoteControl::send_minimal_http_reply(int fd, void *buff, int nbytes) {
	snprintf(reply_buffer, reply_buffer_length,
		"HTTP/1.1 200 OK\nServer: nauto_server/%d.0\n"
		"Content-Length: %d\nConnection: close\nContent-Type: text/html\n\n", 
		server_version, nbytes); /* header + a blank line */
	write(fd, reply_buffer, strlen(reply_buffer));
	write(fd, buff, nbytes);
	return true;
}

/* nbytes indicates how many bytes will be sent immediately after this call */
bool RemoteControl::send_minimal_http_image(int fd, std::vector<uchar> &img_buff) {
	int nbytes = img_buff.size();
	snprintf(reply_buffer, reply_buffer_length, 
		"HTTP/1.1 200 OK\r\nContent-Type: image/jpg\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n", nbytes);
	// snprintf(reply_buffer, reply_buffer_length,
		// "HTTP/1.1 200 OK\nServer: nauto_server/%d.0\n"
		// "Content-Length: %d\nConnection: close\nContent-Type: text/html\n\n", 
		// server_version, nbytes); /* header + a blank line */
	int index = strlen(reply_buffer);
printf("index=%d nbytes=%d reply_buffer_length=%d\n", index, nbytes, reply_buffer_length);
	if((index + nbytes) > reply_buffer_length) return false;
	unsigned char *p = (unsigned char *)&reply_buffer[index];
	unsigned char *p0 = p;
	std::vector<uchar>::const_iterator cit, clast = img_buff.end();
	for(cit=img_buff.begin();cit!=clast;) *p++ = *cit++;
printf("sending http buffer [%s]\n", reply_buffer);
printf("write(fd=%d, buff=%p, nwrite=%d);\n", fd, reply_buffer, index);
	write(fd, reply_buffer, index);
printf("write(fd=%d, buff=%p, nwrite=%d);\n", fd, p0, nbytes);
	write(fd, p0, nbytes);
	return true;
}

bool RemoteControl::close() {
	std::vector<struct pollfd>::const_iterator it, last = poll_fds.end();
	for(it = poll_fds.begin();it != last;++it) {
		::close(it->fd);
	}
	return true;
}

