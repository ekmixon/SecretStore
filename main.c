#include <stdio.h>
#include <stdlib.h>
#include <db.h>
#include "base64.h"

//start_deamon
#include <sys/socket.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>

#define KEY_SIZE 32
#define INCOMING_BUF_SIZE 512
#define DB_PATH "store.db"

int
db_open(DB **store, const char* filename){
	int db_err;

	//DB_CREATE - Crete a new DB if one doesn't exist.
	//DB_INIT_CDB - create a a non-deadlocking multi-reader single writer access.
	//DB_INIT_MPOOl - init a shared memory buffer pool sub-system.
	//docs - http://docs.oracle.com/cd/E17076_02/html/programmer_reference/cam.html
	u_int32_t flags = DB_CREATE | DB_INIT_CDB | DB_INIT_MPOOL;

	db_err = db_create(store, NULL, 0);
	if(db_err == 0){
		db_err = (**store).open(*store,
							 NULL,
							 filename,
							 NULL,
							 DB_BTREE,
							 flags,
							 0);
	}
	return db_err;
}

struct proto_handler_args{
	int sock;
	DB* store;
};

char*
request_handler(DB* store, char* req){
	printf("Got a request: '%s'\n", req);
	return "unimplemented";
}

void*
proto_handler(void* proto_handler_args){
	int sock= ((struct proto_handler_args*)proto_handler_args)->sock;
	DB* store=((struct proto_handler_args*)proto_handler_args)->store;

	int b64_size = base64_size(KEY_SIZE);
	char* buffer = (char*)malloc(b64_size); // TODO who will free
	memset(buffer, 0, b64_size);

	int n;
	char c;
	int buf_i=0;
	printf("About to read\n");
    while(1) {
    	n = read(sock, &c, 1);
    	if(n == 0) continue; // maybe no data was sent yet, continue waiting
    	if(n == -1) { printf("Error reading."); break;  } // TODO

    	if(buf_i >=b64_size){
    		// out of bounds, TODO error
    		break;
    	}

    	if(c == '\n'){
    		if(buffer[buf_i-1]=='\r'){
    			buffer[buf_i-1]=0x00;
    		}

    		// performing a return from the start function of any thread results in an implicit call to pthread_exit()
    		return request_handler(store, buffer);
    	}
    	buffer[buf_i]=c;
        buf_i++;
    }

    printf("%s", buffer);
    bzero(buffer,b64_size);
}

//
int
start_daemon(DB *store, int port, int thread_count){
	int sock, con;
	struct sockaddr_in my_addr;
	struct sockaddr client;
	int optval = 1;
	int client_size;
	pthread_t * threads;

	struct proto_handler_args args; // thread args
	args.store=store;

	threads = malloc(sizeof(pthread_t) * thread_count);

	if(!threads){
		return 1;
	}

	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(port);
	my_addr.sin_addr.s_addr = INADDR_ANY;

	sock = socket( PF_INET, SOCK_STREAM, 0 );
	setsockopt( sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval) );
	bind( sock, (struct sockaddr* )&my_addr, sizeof( struct sockaddr_in ) );
	//10 BACK LOG'ed requests.
	listen(sock, 10);
	while(1){
		con = accept( sock, (struct sockaddr *)&client, &client_size );
		args.sock=con;

		pthread_create(&threads[0], NULL, proto_handler, (void*)&args);
	}

	return 0;
}

char * get_nonce(int size){
	char * buf;
	char * ret;
	FILE * urand = fopen("/dev/urandom","r");
	buf = (char *)malloc(size);
	fgets(buf, size, urand);
	ret = base64_encode(buf, size);
	free(buf);
	close(urand);
	return ret;
}

char *
get_secret(DB *store, char* key){
	DBT lookup_key, secret;
	memset(&lookup_key, 0, sizeof(DBT));
	memset(&secret, 0, sizeof(DBT));

	lookup_key.data=key;
	lookup_key.size=strlen(key) + 1;

	store->get(store, NULL, &lookup_key, &secret, 0);

	return secret.data;
}

char *
new_secret(DB *store, int size){
	DBT key, secret;
	int db_err;

	memset(&key, 0, sizeof(DBT));
	memset(&secret, 0, sizeof(DBT));

	key.data = get_nonce(KEY_SIZE);
	key.size = strlen(key.data) + 1;

	secret.data = get_nonce(size);
	secret.size = strlen(secret.data) + 1;

	do{
		db_err = store->put(store, NULL, &key, &secret, DB_NOOVERWRITE);
		if(db_err == DB_KEYEXIST){
			//Birthday paradox, we may need a new key.
			free(key.data);
			key.data = get_nonce(KEY_SIZE);
		}
	}while(db_err == DB_KEYEXIST);

	free(secret.data);
	return key.data;
}

void
help(){
	printf("You need help son!\n");
}

int
main(int argc, char *argv[]){
	DB *store;
	char * key;
	int db_err;
	int key_size;

	db_err = db_open(&store, DB_PATH);
	if(db_err != 0){
		printf("Error opening database at '%s'\n", DB_PATH);

		switch(db_err){
		case DB_LOCK_DEADLOCK:
			printf("DB_LOCK_DEADLOCK\n");
			break;
		case DB_LOCK_NOTGRANTED:
			printf("DB_LOCK_NOTGRANTED\n");
			break;
		case ENOENT:
			printf("ENOENT\n");
			break;
		case DB_OLD_VERSION:
			printf("DB_OLD_VERSION\n");
			break;
		case EEXIST:
			printf("EEXIST\n");
			break;
		case EINVAL:
			printf("EINVAL\n");
			break;
		case DB_REP_HANDLE_DEAD:
			printf("DB_REP_HANDLE_DEAD\n");
			break;
		}
		//error...
		return 1;
	}

	start_daemon(store, 50100, 5);

	/* TODO: move to handle_request()
	if(argc == 2){
		if(strlen(argv[1]) >= KEY_SIZE){
			key=get_secret(store, argv[1]);
			if(!key){
				printf("Not found.\n");
			}else{
				printf("%s\n", key);
			}
		}else{
			key_size = atoi(argv[1]);
			if(key_size > 0){
				key = new_secret(store, key_size);
				printf("%s\n", key);
				free(key);
			}else{
				help();
			}
		}
	}else{
		help();
	}*/

	if(store){
		store->close(store, 0);
	}
	return 0;
}


