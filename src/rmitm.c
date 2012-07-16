
#include "config.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy
#include <unistd.h>	//read
#include <getopt.h>
#include <errno.h>
#include <poll.h>
#include <sys/types.h>	//open
#include <sys/stat.h>	//open
#include <fcntl.h>	//open

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "log.h"
#include "trans_rdma.h"

#define CHUNK_SIZE 1024*1024
#define RECV_NUM 4

#define TEST_Z(x)  do { if ( (x)) { ERROR_LOG("error: " #x " failed (returned non-zero)." ); exit(-1); }} while (0)
#define TEST_NZ(x) do { if (!(x)) { ERROR_LOG("error: " #x " failed (returned zero/null)."); exit(-1); }} while (0)

struct privatedata {
	FILE *logfd;
	libercat_trans_t *o_trans;
	struct ibv_mr *mr;
	libercat_data_t **first_rdata;
	struct datalock *first_datalock;
	pthread_mutex_t *lock;
	pthread_mutex_t *o_lock;
};

struct datalock {
	libercat_data_t *data;
	pthread_mutex_t *lock;
	pthread_cond_t *cond;
};

void callback_recv(libercat_trans_t *, void*);

void callback_send(libercat_trans_t *trans, void *arg) {
	struct datalock *datalock = arg;
	struct privatedata *priv = trans->private_data;
	if (!datalock || !priv) {
		ERROR_LOG("no callback_arg?");
		return;
	}

	libercat_post_recv(priv->o_trans, &datalock->data, priv->mr, callback_recv, datalock);
}

void callback_disconnect(libercat_trans_t *trans) {
	if (!trans->private_data)
		return;

	struct datalock *datalock = trans->private_data;
	pthread_mutex_lock(datalock->lock);
	pthread_cond_signal(datalock->cond);
	pthread_mutex_unlock(datalock->lock);
}

void callback_recv(libercat_trans_t *trans, void *arg) {
	struct datalock *datalock = arg;
	struct privatedata *priv = trans->private_data;
	if (!datalock || !priv) {
		ERROR_LOG("no callback_arg?");
		return;
	}

	libercat_data_t *data = datalock->data;

	fwrite(data->data, data->size, sizeof(char), priv->logfd);
	fwrite("\11\11\11\11\11\11\11\11\11\11\11\11\11\11\11\11", 16, sizeof(char), priv->logfd);
	fflush(priv->logfd);

	libercat_post_send(priv->o_trans, data, priv->mr, callback_send, datalock);
}

void print_help(char **argv) {
	printf("Usage: %s -s port -c addr port\n", argv[0]);
}

void* handle_trans(void *arg) {
	libercat_trans_t *trans = arg;
	struct privatedata *priv;
	libercat_trans_t *o_trans;
	struct ibv_mr *mr;
	int i;

	TEST_NZ(priv = trans->private_data);
	TEST_NZ(o_trans = priv->o_trans);
	TEST_NZ(mr = priv->mr);

	for (i=0; i<RECV_NUM; i++)
		TEST_Z(libercat_post_recv(trans, &(priv->first_rdata[i]), mr, callback_recv, &(priv->first_datalock[i])));


	if (trans->server) {
		// (tell the other we're done and )wait for the other
		pthread_mutex_unlock(priv->o_lock);
		pthread_mutex_lock(priv->lock);
		TEST_Z(libercat_finalize_accept(trans));
	} else {
		TEST_Z(libercat_finalize_connect(trans));
		// tell the other we're done(and wait for the other)
		pthread_mutex_unlock(priv->o_lock);
		pthread_mutex_lock(priv->lock);
	}

	struct pollfd pollfd_stdin;
	pollfd_stdin.fd = 0; // stdin
	pollfd_stdin.events = POLLIN | POLLPRI;
	pollfd_stdin.revents = 0;

	char dumpstr[10];

	while (trans->state == LIBERCAT_CONNECTED) {

		i = poll(&pollfd_stdin, 1, 100);

		if (i == -1)
			break;

		if (i == 0)
			continue;

		read(0, dumpstr, 10);
	}	


	libercat_destroy_trans(&trans);

	pthread_exit(NULL);
}

int main(int argc, char **argv) {


	libercat_trans_t *s_trans;
	libercat_trans_t *child_trans;
	libercat_trans_t *c_trans;

	libercat_trans_attr_t s_attr;
	libercat_trans_attr_t c_attr;

	memset(&s_attr, 0, sizeof(libercat_trans_attr_t));
	memset(&c_attr, 0, sizeof(libercat_trans_attr_t));

	s_attr.server = -1; // put an incorrect value to check if we're either client or server
	c_attr.server = -1;
	// sane values for optional or non-configurable elements
	s_attr.rq_depth = RECV_NUM;
	s_attr.sq_depth = RECV_NUM;
	s_attr.addr.sa_in.sin_family = AF_INET;
	s_attr.disconnect_callback = callback_disconnect;
	c_attr.rq_depth = RECV_NUM;
	c_attr.sq_depth = RECV_NUM;
	c_attr.addr.sa_in.sin_family = AF_INET;
	c_attr.disconnect_callback = callback_disconnect;

	// argument handling
	static struct option long_options[] = {
		{ "client",	required_argument,	0,		'c' },
		{ "server",	required_argument,	0,		's' },
		{ "help",	no_argument,		0,		'h' },
		{ 0,		0,			0,		 0  }
	};

	int option_index = 0;
	int op, last_op;
	last_op = 0;
	while ((op = getopt_long(argc, argv, "-@hvs:S:c:", long_options, &option_index)) != -1) {
		switch(op) {
			case 1: // this means double argument
				if (last_op == 'c') {
					c_attr.addr.sa_in.sin_port = htons(atoi(optarg));
				} else if (last_op == 'S') {
					s_attr.addr.sa_in.sin_port = htons(atoi(optarg));
				} else {
					ERROR_LOG("Failed to parse arguments");
					print_help(argv);
					exit(EINVAL);
				}
				break;
			case '@':
				printf("%s compiled on %s at %s\n", argv[0], __DATE__, __TIME__);
				printf("Release = %s\n", VERSION);
				printf("Release comment = %s\n", VERSION_COMMENT);
				printf("Git HEAD = %s\n", _GIT_HEAD_COMMIT ) ;
				printf("Git Describe = %s\n", _GIT_DESCRIBE ) ;
				exit(0);
			case 'h':
				print_help(argv);
				exit(0);
			case 'v':
				ERROR_LOG("verbose switch not ready just yet, come back later!\n");
				break;
			case 'c':
				c_attr.server = 0;
				inet_pton(AF_INET, optarg, &c_attr.addr.sa_in.sin_addr);
				break;
			case 's':
				s_attr.server = 10;
				inet_pton(AF_INET, "0.0.0.0", &s_attr.addr.sa_in.sin_addr);
				s_attr.addr.sa_in.sin_port = htons(atoi(optarg));
				break;
			case 'S':
				s_attr.server = 10;
				inet_pton(AF_INET, optarg, &s_attr.addr.sa_in.sin_addr);
				break;
			default:
				ERROR_LOG("Failed to parse arguments");
				print_help(argv);
				exit(EINVAL);
		}
		last_op = op;
	}

	if (c_attr.server == -1 || s_attr.server == -1) {
		ERROR_LOG("must have both client and server!");
		print_help(argv);
		exit(EINVAL);
	}


	// server init
	TEST_Z(libercat_init(&s_trans, &s_attr));

	if (!s_trans)
		exit(-1);

	TEST_Z(libercat_bind_server(s_trans));
	child_trans = libercat_accept_one(s_trans);
	
	TEST_Z(libercat_start_cm_thread(s_trans));
	
	// got a client, start our own client before we finalize the server's connection

	c_attr.pd = child_trans->pd;

	TEST_Z(libercat_init(&c_trans, &c_attr));

	if (!c_trans)
		exit(-1);

	TEST_Z(libercat_connect(c_trans));


	// set up data_t elements and mr (needs to be common for both as well)

	uint8_t *rdmabuf;
	struct ibv_mr *mr;

	const size_t mr_size = 2*(RECV_NUM+1)*CHUNK_SIZE*sizeof(char);

	TEST_NZ(rdmabuf = malloc(mr_size));
	memset(rdmabuf, 0, mr_size);
	//FIXME that's not possible, can only reg it once -- need to use the same pd for both trans
	TEST_NZ(mr = libercat_reg_mr(c_trans, rdmabuf, mr_size, IBV_ACCESS_LOCAL_WRITE));


	libercat_data_t **rdata;
	struct datalock *datalock;
	int i;

	TEST_NZ(rdata = malloc(RECV_NUM*sizeof(libercat_data_t*)));
	TEST_NZ(datalock = malloc(RECV_NUM*sizeof(struct datalock)));

	for (i=0; i < 2*RECV_NUM; i++) {
		TEST_NZ(rdata[i] = malloc(sizeof(libercat_data_t)));
		rdata[i]->data=rdmabuf+i*CHUNK_SIZE*sizeof(char);
		rdata[i]->max_size=CHUNK_SIZE*sizeof(char);
		datalock[i].data = rdata[i];
//		datalock[i].lock = &lock;
//		datalock[i].cond = &cond;
	}

	// set up the data needed to communicate
	TEST_NZ(child_trans->private_data = malloc(sizeof(struct privatedata)));
	TEST_NZ(c_trans->private_data = malloc(sizeof(struct privatedata)));
	struct privatedata *s_priv, *c_priv;
	s_priv = child_trans->private_data;
	c_priv = c_trans->private_data;

	s_priv->logfd = fopen("/tmp/rcat_s_log", "w+");
	s_priv->o_trans = c_trans;
	c_priv->logfd = fopen("/tmp/rcat_c_log", "w+");
	c_priv->o_trans = child_trans;

	s_priv->first_rdata    = rdata;
	s_priv->first_datalock = datalock;
	s_priv->mr             = mr;
	TEST_NZ(s_priv->lock = malloc(sizeof(pthread_mutex_t)));
	pthread_mutex_init(s_priv->lock, NULL);
	c_priv->first_rdata    = rdata + RECV_NUM;
	c_priv->first_datalock = datalock + RECV_NUM;
	c_priv->mr             = mr;
	TEST_NZ(c_priv->lock = malloc(sizeof(pthread_mutex_t)));
	pthread_mutex_init(c_priv->lock, NULL);
	s_priv->o_lock         = c_priv->lock;
	c_priv->o_lock           = s_priv->lock;


	pthread_t s_threadid, c_threadid;
	pthread_mutex_lock(c_priv->lock);
	pthread_mutex_lock(s_priv->lock);
	pthread_create(&s_threadid, NULL, handle_trans, child_trans);
	pthread_create(&c_threadid, NULL, handle_trans, c_trans);

	pthread_join(s_threadid, NULL);
	pthread_join(c_threadid, NULL);

	printf("closing stuff!\n");

	fclose(s_priv->logfd);
	fclose(c_priv->logfd);
	free(c_priv->lock);
	free(s_priv->lock);
	free(c_priv);
	free(s_priv);
	for (i=0; i<2*RECV_NUM; i++)
		free(rdata[i]);
	free(rdata);
	free(datalock);
	free(rdmabuf);

	libercat_destroy_trans(&s_trans);

	return 0;
}

