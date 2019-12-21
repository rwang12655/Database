README

Files of interest: server.c and db.c

Server.c Overview:
This file's main purpose is to create clients, to handle them appropriately by adding and removing them from the circular doubly linked list of threads, and to handle cleanup properly. If the server is ever to be terminated (achievable only through ctrl-d 
since the code I wrote masks both SIGPIPE and SIGINT), the program removes and cleans up any currently running threads in a 
thread and memory-safe way. Throughout the program, mutex's are used to ensure that the client thread list and server struct
are modified safely.

Code Breakdown:

1. client_control_stop(), client_control_release()
Called by the main thread of the program whenever an "s" (stop) or a  "g" (go) is entered into the command line. 

2. run_client(void *arg)
Called by a client thread in the pthread_create function. Because of how it's called, its input is a client_t pointer that
has been casted to a null pointer. In run client, the void argument is unpacked and if the server is still accepting clients,
the client is added to the client thread list (a circular doubly linked list), and then it is prepped to handle input. The
whole process is done in a memory safe way and without deadlock by locking the appropriate mutex at each state and by
anticipating cancellation points ahead of time (handled by pthread_set_cancelstate)

3. delete_all()
Locks the client thread list mutex and cancels all currently active threads.

4. thread_cleanup(void *arg)
Called by pthread_cleanup_pop after being pushed onto the calling thread's stack of cleanup handlers. Since it's called by 
pthread_cleanup_push(void (*routine)(void *), void *arg), the input parameter void *arg for thread_cleanup is a client pointer
that has been casted to a void pointer. After unpacking the void pointer, the function locks the appropriate mutexes, removes the
target client from the client thread list, and frees any resources associated with that client.

5. sig_handler_constructor()
Instantiate a signal handler, adds SIGINT to its sigset, and calls pthread_create.

6. monitor_signal(void *arg)
Called by pthread_create in the sig_handler_constructor(), the input parameter is a signal_handler pointer casted to null. 
monitor_signal is responsible for waiting on a SIGINT signal and calling the delete_all() function when the signal is received.

7. sig_handler_destructor(sig_handler_t *sighandler)
Destroys the signal handler.


db.c Overview
Implemented fine-grained locking, meaning that instead of implementing course-grained locking (locking the entire 
binary tree whenever a thread wished to modify/read it), I added a lock to the node struct and would lock as threads made 
calls that iterated down the tree (each individual node would be locked only if the calling thread needed to use it). 
This allows multiple threads to concurrently modify the tree in a safe way.


How to run:
Use the Makefile to compile the code (run "make clean all" and a "server" and "client" executable will be created). You can
begin by running the server on an arbitrary port and then opening up a new terminal window and running the client executable (This can be done as many times as desired, the code is capable of handling multiple clients). A few scripts have been included 
and they can be run as follows from the command line: ./client <hostname> <port> [script] [occurrences]. 

