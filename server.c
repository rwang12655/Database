#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "./comm.h"
#include "./db.h"
#ifdef __APPLE__
#include "pthread_OSX.h"
#endif

#define STOPPED 1
#define CONTINUE 0

/*
 * Use the variables in this struct to synchronize your main thread with client
 * threads. Note that all client threads must have terminated before you clean
 * up the database.
 */
typedef struct server_control {
    pthread_mutex_t server_mutex;
    pthread_cond_t server_cond;
    int num_client_threads;
} server_control_t;

/*
 * Controls when the clients in the client thread list should be stopped and
 * let go.
 */
typedef struct client_control {
    pthread_mutex_t go_mutex;
    pthread_cond_t go;
    int stopped;
} client_control_t;

/*
 * The encapsulation of a client thread, i.e., the thread that handles
 * commands from clients.
 */
typedef struct client {
    pthread_t thread;
    FILE *cxstr;  // File stream for input and output

    // For client list
    struct client *prev;
    struct client *next;
} client_t;

/*
 * The encapsulation of a thread that handles signals sent to the server.
 * When SIGINT is sent to the server all client threads should be destroyed.
 */
typedef struct sig_handler {
    sigset_t set;
    pthread_t thread;
} sig_handler_t;

// Global variables
client_t *thread_list_head = NULL;
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;
client_control_t client_controller;
server_control_t server_controller;
int server_status;

// Forward declarations
void *run_client(void *arg);
void *monitor_signal(void *arg);
void thread_cleanup(void *arg);
void cleanup_pthread_mutex_unlock(void *arg);

/*
 * Called by client threads to wait until progress is permitted
 */
void client_control_wait() {
    
    // Blocks the calling thread until signaled to release
    int err;
    if((err = pthread_mutex_lock(&client_controller.go_mutex)) != 0){
        handle_error_en(err, "error locking client");
    }
    pthread_cleanup_push(&cleanup_pthread_mutex_unlock,
                         (void*) &client_controller.go_mutex);
    while(client_controller.stopped == STOPPED){
        if((err = pthread_cond_wait(&client_controller.go, &client_controller.go_mutex)) != 0){
            handle_error_en(err, "pthread_cond_wait error");
        }
    }
    pthread_cleanup_pop(1);
}

/*
 * Called by main thread to stop client threads
 */
void client_control_stop() {
    
    // Ensures next call of client_control_wait will block
    int err;
    if((err = pthread_mutex_lock(&client_controller.go_mutex)) != 0){
        handle_error_en(err, "error locking client");
    }
    client_controller.stopped = STOPPED;
    if((err = pthread_mutex_unlock(&client_controller.go_mutex)) != 0){
        handle_error_en(err, "error unlocking client");
    }
}

/*
 * Called by main thread to resume client threads
 */
void client_control_release() {
    
    // Continue clients that are blocked within client_control_wait()
    int err;
    
    if((err = pthread_mutex_lock(&client_controller.go_mutex)) != 0){
        handle_error_en(err, "error locking client");
    }
    client_controller.stopped = CONTINUE;
    if((err = pthread_cond_broadcast(&client_controller.go)) != 0){
        handle_error_en(err, "error broadcasting");
    }
   if((err = pthread_mutex_unlock(&client_controller.go_mutex)) != 0){
       handle_error_en(err, "error unlocking ");
   }
}

/*
 * Called by listener (in comm.c) to create a new client thread
 */
void client_constructor(FILE *cxstr) {
    
    // Allocate memory and set fields for a new client
    int err;
    client_t *new_client;
    if((new_client = malloc(sizeof(client_t))) == NULL){
        perror("Error with malloc\n");
        exit(1);
    }
    new_client->cxstr = cxstr;
    new_client->next = NULL;
    new_client->prev = NULL;

    // Creates the new client thread running the run_client routine.
    if((err = pthread_create(&new_client->thread, NULL, &run_client, (void*) new_client)) != 0){
        handle_error_en(err, "error creating thread");
    }
}

/*
 * Frees all resources associated with a client.
 */
void client_destructor(client_t *client) {
    comm_shutdown(client->cxstr);
    free(client);
}

/*
 * Code executed by a client thread
 */
void *run_client(void *arg) {
    
    // Unpack the argument
    client_t *client = (client_t*) arg;
    int err;

    pthread_cleanup_push(thread_cleanup, arg);
    
    // Make sure that the server is still accepting clients.
    if(server_status != STOPPED){
        
        if((err = pthread_mutex_lock(&server_controller.server_mutex)) != 0){
            handle_error_en(err, "error locking");
        }
        if((err = pthread_mutex_lock(&thread_list_mutex)) != 0){
            handle_error_en(err, "error locking");
        }
        
        // Adds client to the client list
        if(thread_list_head == NULL){
            thread_list_head = client;
            thread_list_head->next = thread_list_head;
            thread_list_head->prev = thread_list_head;
        } else {
            client->next = thread_list_head;
            client->prev = thread_list_head->prev;
            
            // If thread list contains only one client
            if(thread_list_head->next == thread_list_head){
                thread_list_head->next = client;
            }
            
            // Multiple clients already in threadlist
            else {
                thread_list_head->prev->next = client;
            }
            thread_list_head->prev = client;
        }
        
        if((err = pthread_mutex_unlock(&thread_list_mutex)) != 0){
            handle_error_en(err, "error unlocking ");
        }
        server_controller.num_client_threads++;
        if((err = pthread_mutex_unlock(&server_controller.server_mutex)) != 0){
            handle_error_en(err, "error unlocking ");
        }

        // Loop through comm_serv, a cancellation point
        // thread_cleanup has been pushed already
        char cmd[512];
        char resp[512];
        while(comm_serve(client->cxstr, resp, cmd) != -1){
            client_control_wait();
            interpret_command(cmd, resp, sizeof(char) * 512);
        }
    }
    
    // Pop thread_cleanup and disable canceling
    if((err = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, 0)) != 0){
        handle_error_en(err, "error setting cancel state");
    }
    pthread_cleanup_pop(1);
    
    return NULL;
}

/*
 * Cancels every thread in the client thread list with the
 * pthread_cancel function.
 */
void delete_all() {
    
    // Prevent modifications
    int err;
    
    if((err = pthread_mutex_lock(&thread_list_mutex)) != 0){
        handle_error_en(err, "error locking");
    }
    if(thread_list_head == NULL){
        if((err = pthread_mutex_unlock(&thread_list_mutex)) != 0){
            handle_error_en(err, "error unlocking ");
        }
        return;
    }
    client_t *temp = thread_list_head->next;
    while(temp != thread_list_head){
        client_t *to_cancel = temp;
        temp = temp->next;
        if((err = pthread_cancel(to_cancel->thread)) != 0){
            handle_error_en(err, "error canceling thread");
        }
    }
    
    // Cancel the head (doesn't get hit during the while loop)
    if((err = pthread_cancel(thread_list_head->thread)) != 0){
        handle_error_en(err, "error canceling thread");
    }
    
    // Re-allow modifications
    if((err = pthread_mutex_unlock(&thread_list_mutex)) != 0){
        handle_error_en(err, "error unlocking ");
    }
}

/*
 * Cleanup routine for client threads, called on cancels and exit.
 */
void thread_cleanup(void *arg) {
    
    // Casting to client_t type
    client_t *client = (client_t*) arg;
    int err;
    
    if((err = pthread_mutex_lock(&server_controller.server_mutex)) != 0){
        handle_error_en(err, "error locking");
    }
    if((err = pthread_mutex_lock(&thread_list_mutex)) != 0){
        handle_error_en(err, "error locking");
    }
    
    if(server_controller.num_client_threads != 0){

        client_t *curr = thread_list_head;
        client_t *prev = NULL;
        
        // Loop through the client list using curr
        while(curr != client){
            prev = curr;
            curr = curr->next;
        }
        
        // Handles case where thread is the head
        if(curr->next == thread_list_head && prev == NULL){
            thread_list_head = NULL;
        }
        
        // If its the first node
        else if(curr == thread_list_head){
            prev = thread_list_head->prev;
            thread_list_head = thread_list_head->next;
            prev->next = thread_list_head;
            thread_list_head->prev = prev;
        }
        // If its the last node
        else if(curr->next == thread_list_head){
            prev->next = thread_list_head;
            thread_list_head->prev = prev;
        }
        else {
            curr->prev->next = curr->next;
            curr->next->prev = curr->prev;
        }
        client_destructor(curr);
        server_controller.num_client_threads--;

        // Signal to the main thread that the threadlist is empty
        if(server_controller.num_client_threads == 0){
            if((err = pthread_cond_signal(&server_controller.server_cond)) != 0){
                handle_error_en(err, "error signalling ");
            }
        }
    }
    if((err = pthread_mutex_unlock(&thread_list_mutex)) != 0){
        handle_error_en(err, "error unlocking ");
    }
    if((err = pthread_mutex_unlock(&server_controller.server_mutex)) != 0){
        handle_error_en(err, "error unlocking ");
    }
}

/*
 * Monitors for SIGINT and cancels all threads if received
 */
void *monitor_signal(void *arg) {
    int signal;
    sig_handler_t *sig_handler = (sig_handler_t*) arg;
    
    while(1){
        sigwait(&sig_handler->set, &signal);
        if(signal == SIGINT){
            fprintf(stderr, "SIGINT received, cancelling all clients\n");
            delete_all();
        } else {
            fprintf(stderr, "Error with SIGINT handling\n");
        }
    }
    return NULL;
}

/*
 * Initializes the signal_handler and sets the sigset
 */
sig_handler_t *sig_handler_constructor() {

    int err;
    sig_handler_t *signal_handler;
    if((signal_handler = malloc(sizeof(sig_handler_t))) == NULL){
        perror("Error with malloc");
        exit(1);
    }
    
    if((err = sigemptyset(&signal_handler->set)) == -1){
        fprintf(stderr, "Error with sigemptyset\n");
        exit(1);
    }
    if((err = sigaddset(&signal_handler->set, SIGINT)) == -1){
        fprintf(stderr, "Error with sigemptyset\n");
        exit(1);
    }
    
    if((err = pthread_create(&signal_handler->thread, NULL, monitor_signal, (void*) signal_handler)) != 0){
        handle_error_en(err, "error creating thread");
    }
    
    return signal_handler;
}

/*
 * Free any resources allocated in sig_handler_constructor.
 */
void sig_handler_destructor(sig_handler_t *sighandler) {
    int err;
    if((err = pthread_cancel(sighandler->thread)) != 0){
        handle_error_en(err, "error canceling thread");
    }
    free(sighandler);
}
/*
 * Function used by pthread_cleanup_push to unlock mutex
 */
void cleanup_pthread_mutex_unlock(void *arg) {
    int err;
    if((err = pthread_mutex_unlock((pthread_mutex_t *) arg)) != 0){
        handle_error_en(err, "error unlocking ");
    }
}

int main(int argc, char *argv[]) {
    
    if(argc != 2){
        fprintf(stderr, "Usage: cs0330_db_demo_server <port>\n");
        exit(1);
    }
    
    // Initializer client and server controller
    int port = atoi(argv[1]);
    int err;
    client_controller.stopped = 0;
    if((err = pthread_mutex_init(&client_controller.go_mutex, NULL)) != 0){
        handle_error_en(err, "error initializing");
    }
    if((err = pthread_cond_init(&client_controller.go, NULL)) != 0){
        handle_error_en(err, "error initializing");
    }
    
    server_controller.num_client_threads = 0;
    server_status = CONTINUE;
    if((err = pthread_mutex_init(&server_controller.server_mutex, NULL)) != 0){
        handle_error_en(err, "error initializing");
    }
    if((err = pthread_cond_init(&server_controller.server_cond, NULL)) != 0){
        handle_error_en(err, "error initializing");
    }
    
    // Step 1: Set up the signal handler.
    sigset_t pipe_handling;

    if((err = sigemptyset(&pipe_handling)) == -1){
        fprintf(stderr, "Error with sigemptyset\n");
        exit(1);
    }
    if((err = sigaddset(&pipe_handling, SIGPIPE)) == -1){
        fprintf(stderr, "Error with sigemptyset\n");
        exit(1);
    }
    if((err = sigaddset(&pipe_handling, SIGINT)) == -1){
        fprintf(stderr, "Error with sigaddset\n");
        exit(1);
    }
    

    if((err = pthread_sigmask(SIG_BLOCK, &pipe_handling, NULL)) != 0){
        handle_error_en(err, "error sigmasking");
    }
    sig_handler_t *sig_handler = sig_handler_constructor();
    
    // Step 2: Start a listener thread for clients (see start_listener in
    //       comm.c).
    pthread_t listener = start_listener(port, client_constructor);
    
    // Step 3: Loop for command line input and handle accordingly until EOF.
    char buffer[BUFSIZ];
    while(read(STDIN_FILENO, buffer, BUFSIZ) != 0){
        char cmd = buffer[0];
        char filebuffer[1024];
        switch (cmd) {
            case 'p':
                sscanf(&buffer[1], "%s", filebuffer);
                db_print(filebuffer);
                break;
            case 'g':
                fprintf(stderr, "releasing testing\n");
                client_control_release();
                break;
            case 's':
                fprintf(stderr, "stopping testing\n");
                client_control_stop();
                break;
            default:
                fprintf(stderr, "ill-formed command");
        }
    }
    
    fprintf(stderr, "Exiting database\n");
    sig_handler_destructor(sig_handler);

    if((err = pthread_mutex_lock(&server_controller.server_mutex)) != 0){
        handle_error_en(err, "error locking");
    }
    
    pthread_cleanup_push(&cleanup_pthread_mutex_unlock,
                         (void*) &server_controller.server_mutex);
    
    // Ensure that server won't accept more clients after delete_all
    server_status = STOPPED;
    delete_all();

    // Wait on pthread_cond_signal from thread_cleanup
    while (server_controller.num_client_threads != 0) {
        if((err = pthread_cond_wait(&server_controller.server_cond, &server_controller.server_mutex)) != 0){
            handle_error_en(err, "error during cond_wait");
        }
    }
    pthread_cleanup_pop(1);
    db_cleanup();
    
    if((err = pthread_cancel(listener)) != 0){
        handle_error_en(err, "error cancelling the listener");
    }
    
    return 0;
}
