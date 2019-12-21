#include "./db.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLEN 256

typedef enum {l_read, l_write} lock_t;

#define lock(lt, lk) ((lt) == l_read) ? pthread_rwlock_rdlock(lk) : pthread_rwlock_wrlock(lk)

// The root node of the binary tree, unlike all
// other nodes in the tree, this one is never
// freed (it's allocated in the data region).
node_t head = {"", "", 0, 0, PTHREAD_RWLOCK_INITIALIZER};

node_t *node_constructor(char *arg_name, char *arg_value, node_t *arg_left,
                         node_t *arg_right) {
    size_t name_len = strlen(arg_name);
    size_t val_len = strlen(arg_value);

    if (name_len > MAXLEN || val_len > MAXLEN) return 0;

    node_t *new_node = (node_t *)malloc(sizeof(node_t));

    if (new_node == 0) return 0;

    if ((new_node->name = (char *)malloc(name_len + 1)) == 0) {
        free(new_node);
        return 0;
    }

    if ((new_node->value = (char *)malloc(val_len + 1)) == 0) {
        free(new_node->name);
        free(new_node);
        return 0;
    }

    if ((snprintf(new_node->name, MAXLEN, "%s", arg_name)) < 0) {
        free(new_node->value);
        free(new_node->name);
        free(new_node);
        return 0;
    }
    else if ((snprintf(new_node->value, MAXLEN, "%s", arg_value)) < 0) {
        free(new_node->value);
        free(new_node->name);
        free(new_node);
        return 0;
    }

    if(pthread_rwlock_init(&new_node->lock, NULL) != 0){
        free(new_node->value);
        free(new_node->name);
        free(new_node);
        return 0;
    }
    
    new_node->lchild = arg_left;
    new_node->rchild = arg_right;
    return new_node;
}

void node_destructor(node_t *node) {
    int err;
    if (node->name != 0) free(node->name);
    if (node->value != 0) free(node->value);
    if((err = pthread_rwlock_destroy(&node->lock)) != 0){
        
    }
    free(node);
}

node_t *search(char *, node_t *, node_t **, lock_t);

void db_query(char *name, char *result, int len) {
    node_t *target;
    
    // Lock head so the parent is locked upon entry of search
    lock(l_read, &head.lock);
    target = search(name, &head, 0, l_read);

    // Unlock only if target is found, otherwise no need
    if (target == 0) {
        snprintf(result, len, "not found");
        return;
    } else {
        snprintf(result, len, "%s", target->value);
        pthread_rwlock_unlock(&target->lock);
        return;
    }
}

int db_add(char *name, char *value) {
    node_t *parent;
    node_t *target;
    node_t *newnode;

    // Make sure parent (head) is locked upon entry of search
    lock(l_write, &head.lock);
    if ((target = search(name, &head, &parent, l_write)) != 0) {
        pthread_rwlock_unlock(&target->lock);
        pthread_rwlock_unlock(&parent->lock);
        return (0);
    }

    newnode = node_constructor(name, value, 0, 0);
    if (strcmp(name, parent->name) < 0){
        parent->lchild = newnode;
    } else{
        parent->rchild = newnode;
    }
    
    pthread_rwlock_unlock(&parent->lock);
    return (1);
}

int db_remove(char *name) {
    node_t *parent;
    node_t *dnode;
    node_t *next;

    // Make sure parent (head) is locked upon entry of search
    lock(l_write, &head.lock);
    if ((dnode = search(name, &head, &parent, l_write)) == 0) {
        
        // If not found
        pthread_rwlock_unlock(&parent->lock);
        return (0);
    }

    // If found and right child is null
    if (dnode->rchild == 0) {
        if (strcmp(dnode->name, parent->name) < 0)
            parent->lchild = dnode->lchild;
        else
            parent->rchild = dnode->lchild;
        pthread_rwlock_unlock(&dnode->lock);
        node_destructor(dnode);
        pthread_rwlock_unlock(&parent->lock);
    }

    // If found and left child is null
    else if (dnode->lchild == 0) {
        if (strcmp(dnode->name, parent->name) < 0)
            parent->lchild = dnode->rchild;
        else
            parent->rchild = dnode->rchild;
        pthread_rwlock_unlock(&dnode->lock);
        node_destructor(dnode);
        pthread_rwlock_unlock(&parent->lock);
    } else {
        // Find the lexicographically smallest node in the right subtree and
        // replace the node to be deleted with that node. This new node thus is
        // lexicographically smaller than all nodes in its right subtree, and
        // greater than all nodes in its left subtree

        // No need for parent lock anymore
        pthread_rwlock_unlock(&parent->lock);
        
        next = dnode->rchild;
        node_t **pnext = &dnode->rchild;
        
        lock(l_write, &next->lock);
        
        // work our way down the lchild chain, finding the smallest node
        // in the subtree. Lock and unlock the nodes as we go
        while (next->lchild != 0) {
            node_t *nextl = next->lchild;
            
            lock(l_write, &nextl->lock);
            pnext = &next->lchild;
            
            pthread_rwlock_unlock(&next->lock);
            next = nextl;
        }

        // Replace deleted node with lexicographically smallest in right subtree
        dnode->name = realloc(dnode->name, strlen(next->name) + 1);
        dnode->value = realloc(dnode->value, strlen(next->value) + 1);

        snprintf(dnode->name, MAXLEN, "%s", next->name);
        snprintf(dnode->value, MAXLEN, "%s", next->value);
        
        *pnext = next->rchild;

        pthread_rwlock_unlock(&dnode->lock);
        pthread_rwlock_unlock(&next->lock);
        node_destructor(next);
    }
    return (1);
}

node_t *search(char *name, node_t *parent, node_t **parentpp, lock_t lock) {
    // Search the tree, starting at parent, for a node containing
    // name (the "target node").  Return a pointer to the node,
    // if found, otherwise return 0.  If parentpp is not 0, then it points
    // to a location at which the address of the parent of the target node
    // is stored.  If the target node is not found, the location pointed to
    // by parentpp is set to what would be the the address of the parent of
    // the target node, if it were there.

    node_t *next;
    node_t *result;

    if (strcmp(name, parent->name) < 0) {
        next = parent->lchild;
    } else {
        next = parent->rchild;
    }

    if (next == NULL) {
        result = NULL;
    } else {
        lock(lock, &next->lock);
        if (strcmp(name, next->name) == 0) {
            result = next;
        } else {
            pthread_rwlock_unlock(&parent->lock);
            return search(name, next, parentpp, lock);
        }
    }

    if (parentpp != NULL) {
        *parentpp = parent;
    }
    
    // Handles cases where we don't need the parent node locked
    else {
        pthread_rwlock_unlock(&parent->lock);
    }

    return result;
}

static inline void print_spaces(int lvl, FILE *out) {
    for (int i = 0; i < lvl; i++) {
        fprintf(out, " ");
    }
}

/* Recursively traverses the database tree and prints nodes
 * pre-order. */
void db_print_recurs(node_t *node, int lvl, FILE *out) {
    // print spaces to differentiate levels
    print_spaces(lvl, out);

    // print out the current node
    if (node == NULL) {
        fprintf(out, "(null)\n");
        return;
    }

    if (node == &head) {
        fprintf(out, "(root)\n");
    } else {
        fprintf(out, "%s %s\n", node->name, node->value);
    }

    // Lock the nodes as we go down
    if(node->lchild != NULL){
        lock(l_read, &node->lchild->lock);
    }
    db_print_recurs(node->lchild, lvl + 1, out);
    
    if(node->rchild != NULL){
        lock(l_read, &node->rchild->lock);
    }
    db_print_recurs(node->rchild, lvl + 1, out);
    
    pthread_rwlock_unlock(&node->lock);
}

/* Prints the whole database, using db_print_recurs, to a file with
 * the given filename, or to stdout if the filename is empty or NULL.
 * If the file does not exist, it is created. The file is truncated
 * in all cases.
 *
 * Returns 0 on success, or -1 if the file could not be opened
 * for writing. */
int db_print(char *filename) {
    FILE *out;
    if (filename == NULL) {
        lock(l_read, &head.lock);
        db_print_recurs(&head, 0, stdout);
        return 0;
    }

    // skip over leading whitespace
    while (isspace(*filename)) {
        filename++;
    }

    if (*filename == '\0') {
        lock(l_read, &head.lock);
        db_print_recurs(&head, 0, stdout);
        return 0;
    }

    if ((out = fopen(filename, "w+")) == NULL) {
        return -1;
    }

    lock(l_read, &head.lock);
    db_print_recurs(&head, 0, out);
    fclose(out);

    return 0;
}

/* Recursively destroys node and all its children. */
void db_cleanup_recurs(node_t *node) {
    if (node == NULL) {
        return;
    }

    db_cleanup_recurs(node->lchild);
    db_cleanup_recurs(node->rchild);

    node_destructor(node);
}

/* Destroys all nodes in the database other than the head.
 * No threads should be using the database when this is called. */
void db_cleanup() {
    db_cleanup_recurs(head.lchild);
    db_cleanup_recurs(head.rchild);
}

/* Interprets the given command string and calls the appropriate database
 * function. Writes up to len-1 bytes of the response message string produced
 * by the database to the response buffer. */
void interpret_command(char *command, char *response, int len) {
    char value[MAXLEN];
    char ibuf[MAXLEN];
    char name[MAXLEN];
    int sscanf_ret;

    if (strlen(command) <= 1) {
        snprintf(response, len, "ill-formed command");
        return;
    }

    // which command is it?
    switch (command[0]) {
        case 'q':
            // Query
            sscanf_ret = sscanf(&command[1], "%255s", name);
            if (sscanf_ret < 1) {
                snprintf(response, len, "ill-formed command");
                return;
            }
            db_query(name, response, len);
            if (strlen(response) == 0) {
                snprintf(response, len, "not found");
            }

            return;

        case 'a':
            // Add to the database
            sscanf_ret = sscanf(&command[1], "%255s %255s", name, value);
            if (sscanf_ret < 2) {
                snprintf(response, len, "ill-formed command");
                return;
            }
            if (db_add(name, value)) {
                snprintf(response, len, "added");
            } else {
                snprintf(response, len, "already in database");
            }

            return;

        case 'd':
            // Delete from the database
            sscanf_ret = sscanf(&command[1], "%255s", name);
            if (sscanf_ret < 1) {
                snprintf(response, len, "ill-formed command");
                return;
            }
            if (db_remove(name)) {
                snprintf(response, len, "removed");
            } else {
                snprintf(response, len, "not in database");
            }

            return;

        case 'f':
            // process the commands in a file (silently)
            sscanf_ret = sscanf(&command[1], "%255s", name);
            if (sscanf_ret < 1) {
                snprintf(response, len, "ill-formed command");
                return;
            }

            FILE *finput = fopen(name, "r");
            if (!finput) {
                snprintf(response, len, "bad file name");
                return;
            }
            while (fgets(ibuf, sizeof(ibuf), finput) != 0) {
                pthread_testcancel();  // fgets is not a cancellation point
                interpret_command(ibuf, response, len);
            }
            fclose(finput);
            snprintf(response, len, "file processed");
            return;

        default:
            snprintf(response, len, "ill-formed command");
            return;
    }
}
