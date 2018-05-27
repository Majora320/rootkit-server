#include <linux/limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


ssize_t getline(char **lineptr, size_t *n, FILE *stream);

void *interaction_fn(void *);
void start_terminal(struct sockaddr_in6 addr);
int sockfd;

struct client {
	struct sockaddr_in6 addr;
	const char *name;
};

enum server_message_types {
	SERVER_MSG_CMD = 0
};

enum client_message_types {
	CLIENT_MSG_CONN = 0,
	CLIENT_MSG_DATA = 1,
	CLIENT_MSG_DISCONN = 2
};

// FIXME: add a mutex here
char cwd_buf[PATH_MAX];
// FIXME: and here
struct client *clients = NULL;
static size_t clients_length = 0, clients_cap = 0;

const char *lookup_client_name(struct sockaddr_in6 addr) {
	if (clients == NULL) return NULL;
	
	for (size_t i = 0; i < clients_length; i += sizeof(struct client)) {
		struct client *c = clients + i;
		if (memcmp(&c->addr, &addr, sizeof(struct sockaddr_in6)) == 0)
			return c->name;
	}
	
	return NULL;
}

int lookup_client_addr(const char *name, struct sockaddr_in6 *out) {
	if (clients == NULL) return -1;
	
	for (size_t i = 0; i < clients_length; i += sizeof(struct client)) {
		struct client *c = clients + i;
		if (memcmp(c->name, name, strlen(name)) == 0) {
			*out = c->addr;
			return 0;
		}
	}
	
	return -1;
}

void add_client(struct sockaddr_in6 addr, const char *name) {
	if (clients == NULL) {
		clients = malloc(10 * sizeof(struct client));
		clients_length = 0;
		clients_cap = 10;
	}
	
	++clients_length;
	
	if (clients_length > clients_cap) {
		clients_cap *= 2;
		clients = realloc(clients, clients_cap);
	}
	
	struct client *insert_at = clients + clients_length-1;
	
	struct client insert = {
		.addr = addr,
		.name = name
	};
	
	*insert_at = insert;
}

void remove_client(struct sockaddr_in6 addr) {
	if (clients_length == 0) return;
	clients_length--;
	
	for (size_t i = 0; i < clients_length; i += sizeof(struct client)) {
		struct client *c = clients + i;
		if (memcmp(&c->addr, &addr, sizeof(struct sockaddr_in6)) == 0) {
			for (size_t k = i+1; k < clients_length; k+= sizeof(struct client)) {
				*(clients+k-1) = *(clients+k);
			}
			
			break;
		}
	}
}

void print_clients() {
	for (size_t i = 0; i < clients_length; i += sizeof(struct client)) {
		if (i == 0)
			printf("%s", clients[i].name);
		else
			printf(", %s", clients[i].name);
	}
}

int main(void) {
	sockfd = socket(PF_INET6, SOCK_DGRAM, 0);
	
	struct sockaddr_in6 addr = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(6005),
		.sin6_addr = IN6ADDR_ANY_INIT,
	};
	
	if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		perror("bind");
		exit(1);
	}
	
	
	pthread_t interaction_thread;
	pthread_create(&interaction_thread, NULL, &interaction_fn, NULL);
	
	unsigned char buf[65507];
	while (1) {
		struct sockaddr_in6 client;
		socklen_t client_struct_size = sizeof(client);
		int bytes = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&client, &client_struct_size);
		puts("oh hey a client connected to us");
		
		if (bytes < 3 || !(buf[0] == 0xfc && buf[1] == 0xcf)) {
			continue;
		}
		
		switch (buf[2]) {
		case CLIENT_MSG_CONN:
			if (bytes - 3 == 0)
				continue; // TODO properly handle error and stuff
				
			char *name = malloc(bytes - 3 + 1);
			memcpy(name, buf, bytes - 3);
			name[bytes - 3] = '\0';
			
			add_client(client, name);
			break;
		case CLIENT_MSG_DATA:
			;
			int i;
			for (i = 3; i < bytes && buf[i] != 0; ++i) {
				cwd_buf[i-3] = buf[i];
			}
			
			if (i >= bytes-1) continue;
			
			cwd_buf[i-2] = '\0';
			
			for (int k = i+1; k < bytes; ++k) {
				printf("%c", buf[k]);
			}
			
			break;
		case CLIENT_MSG_DISCONN:
			remove_client(client);
			break;
		}
	}
}

void *interaction_fn(void *unused __attribute__ ((unused))) {
	while (1) {
		printf("Enter a client [empty to list clients]: ");
		char *line = NULL;
		size_t size = 0;
		ssize_t line_length = getline(&line, &size, stdin);
		if (line_length == -1) {
			perror("getline");
			exit(1);
		}
		
		line[line_length-1] = '\0';
		
		if (*line == '\0') {
			if (clients_length == 0) {
				puts("No available clients.");
			} else {	
				printf("Available clients: ");
				print_clients();
				puts("");
			}
		} else {
			struct sockaddr_in6 addr;
			if (lookup_client_addr(line, &addr) < 0) {
				puts("Invalid client.");
				goto free;
			}
			
			start_terminal(addr);
		}
		
	free:
		free(line);
	}
	
	return NULL;
}

void start_terminal(struct sockaddr_in6 addr) {
	while (1) {
		const char *name = lookup_client_name(addr);
		if (name == NULL) {
			puts("Client has disconnected.");
			break;
		}
		
		printf("(%s) %s$ ", name, cwd_buf);
		
		char *line = NULL;
		size_t size = 0;
		ssize_t line_length = getline(&line, &size, stdin);
		if (line_length == -1) {
			perror("getline");
			exit(1);
		}
		
		line[line_length-1] = '\0';
		
		char *packet = malloc(3 + line_length-1);
		
		packet[0] = 0xfc;
		packet[1] = 0xcf;
		packet[2] = SERVER_MSG_CMD;
		memcpy(packet+3, line, line_length-1);
		
		if (sendto(sockfd, packet, 3 + line_length-1, 0, (struct sockaddr *)&addr, sizeof(addr)) == -1)
			perror("sendto");
	}
}