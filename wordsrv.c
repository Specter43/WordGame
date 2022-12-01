#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
    #define PORT 52943
#endif
#define MAX_QUEUE 5

int check_exist(struct client **top, int fd);
void add_player(struct client **top, int fd, struct in_addr addr);
void remove_player(struct game_state *game, struct client **top, int fd, char *function_name);

/* These are some of the function prototypes that we used in our solution 
 * You are not required to write functions that match these prototypes, but
 * you may find the helpful when thinking about operations in your program.
 */
/* Send the message in outbuf to all clients */
void broadcast(struct game_state *game, char *outbuf, int exclusion_fd);
void announce_turn(struct game_state *game);
void announce_winner(struct game_state *game, struct client *winner);
/* Move the current_player pointer to the next active client */
void advance_turn(struct game_state *game);
/* The following are helpers */
int find_network_newline(const char *buf, int n);
int check_read(struct game_state *game, int fd, char *buf, int room, struct client **new_players);
int read_guess(int fd, struct client *p, struct game_state *game, char *username);
int update_guessed(struct game_state *game, char *guess);
int no_guess(struct game_state *game);
int read_username(struct client *p, struct game_state *game, int fd, struct client **new_players);
void remove_new_player(struct client **top, int fd);
void add_new_player(struct client **top, int fd, char *name);
void move_to_game(struct client **new_players, int fd, struct game_state *game, char *name);


/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;

// Check if a player exists according to where they are placed
int check_exist(struct client **top, int fd) {
    struct client *ptr;
    int exist = 0;
    // Loop over the given linked list
    for (ptr = *top; ptr != NULL; ptr = ptr->next) {
        if (ptr->fd == fd) { // Have such user in the linked list
            exist = 1;
            break;
        }
    }
    return exist;
}

/* Add a client to the head of the linked list
 */
void add_player(struct client **top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}

/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset 
 */
void remove_player(struct game_state *game, struct client **top, int fd, char *function_name) {
    struct client **p;
    int cmp;

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next)
    ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Disconnect from %s\n", inet_ntoa((*p)->ipaddr));
        printf("Removing client %d %s during %s\n", fd, inet_ntoa((*p)->ipaddr), function_name);

        // Construct goodbye message if the user has a valid name
        cmp = strcmp((*p)->name, "");
        if (cmp != 0) {
            char bye_message[MAX_MSG] = {'\0'};
            strcat(bye_message, "Goodbye ");
            strcat(bye_message, (*p)->name);
            strcat(bye_message, "\r\n");
            // Broadcast goodbye
            broadcast(game, bye_message, fd);
            announce_turn(game);
        }

        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
        // If the last player is removed, empty current player
        if (game->current_player != NULL && game->head == NULL) {
            game->current_player = NULL;
        } 
    } else {
        fprintf(stderr, "Trying to remove fd %d in %s, but I don't know about it\n", fd, function_name);
    }
}

// Write message to all active players
void broadcast(struct game_state *game, char *outbuf, int exclusion_fd) {
    struct client *ptr;
    // Loop over every active player in current game state
    for (ptr = game->head; ptr != NULL; ptr = ptr->next) {
        if (ptr->fd != exclusion_fd) { // Any player other than the excluded one
            int dp = dprintf(ptr->fd, "%s", outbuf);
            if (dp < 0) { // Disconnection
                remove_player(game ,&(game->head), ptr->fd, "broadcast");
            }
        }
    }
}

// Announce which player's turn to all active players.
void announce_turn(struct game_state *game) {
    int dp;
    struct client *ptr;
    // Loop over every active player in current game state
    for (ptr = game->head; ptr != NULL; ptr = ptr->next) {
        // Construct the message for sockets
        if ((game->current_player)->fd == ptr->fd) { // Current turn player
            dp = dprintf(ptr->fd, "Your guess?\r\n");
            if (dp < 0) { // Disconnection
                remove_player(game, &(game->head), ptr->fd, "announce turn");
            }
            continue;
        } else { // Other players
            dp = dprintf(ptr->fd, "It's %s's turn\r\n", (game->current_player)->name);
            if (dp < 0) { // Disconnection
                remove_player(game, &(game->head), ptr->fd, "announce turn");
            }
        }
    }
}

// Announce winner to all active players.
void announce_winner(struct game_state *game, struct client *winner) {
    struct client *ptr;
    int cmp, dp;
    // Loop over every active player in current game state
    for (ptr = game->head; ptr != NULL; ptr = ptr->next) {
        // Construct the message for sockets
        cmp = strcmp(ptr->name, winner->name);
        if (cmp == 0) { // Current turn player is winner
            dp = dprintf(ptr->fd, "Game over! You win!\n\n\nLet's start a new game\r\n");
            if (dp < 0) { // Disconnection
                remove_player(game, &(game->head), ptr->fd, "announce winner");
            }
        } else { // Other players
            dp = dprintf(ptr->fd, "Game over! %s won!\n\n\nLet's start a new game\r\n", winner->name);
            if (dp < 0) { // Disconnection
                remove_player(game, &(game->head), ptr->fd, "announce winner");
            }
        }
    }
}

// Change the current player to next active player
void advance_turn(struct game_state *game) {
    // Current player is not the back of the linked list
    if ((game->current_player)->next != NULL) {
        game->current_player = game->current_player->next;
    } else { // Current player is the back of the linked list
        game->current_player = game->head;
    }
}

// Helper from lab10 to help read
int find_network_newline(const char *buf, int n) {
    if (*(buf - 1) == '\n' && *(buf - 2) == '\r') {
        return 1;
    }
    return 0;
}

// Helper for read_guess and read_username, error checking for read
int check_read(struct game_state *game, int fd, char *buf, int room, struct client **new_players) {
    int num_read = read(fd, buf, room);
    int exist_in_official;
    if (num_read == 0) { // The player didn't successfully enter input and disconnected
        exist_in_official = check_exist(&(game->head), fd);
        if (exist_in_official == 1) { // Disconnection from official (for read_guess)
            if ((game->current_player)->fd == fd) { // Removing the player who should be guessing
                // Give turn to next active player
                advance_turn(game);
                remove_player(game, &(game->head), fd, "check read");
            } else { // Removing the player who should not be guessing
                remove_player(game, &(game->head), fd, "check read");
            }
        } else if (exist_in_official == 0 && new_players != NULL) { // Disconnection from new_players (for read_username)
            remove_player(game, new_players, fd, "check read");
        }
    } else if (num_read < 0) { // Read is not ok
        perror("read");
        exit(1);
    }
    return num_read;
}

// Clear client's inbuf after operation them
void clear_inbuf(struct client *p, int room) {
    for (int i = 0; i < room; i++) {
        p->inbuf[i] = '\0';
    }
    p->in_ptr = p->inbuf;
}
// Read a valid guess from player
int read_guess(int fd, struct client *p, struct game_state *game, char *username) {
    int dp;
    int nbytes;
    // Receive a guess from user. (Code from lab10)
    if ((nbytes = check_read(game, fd, p->in_ptr, MAX_BUF, NULL)) > 0) {
        p->in_ptr += nbytes;
        int where;
        where = find_network_newline(p->in_ptr, MAX_BUF);
        // Avoid partial reads that creates too many messages for user
        if (where == 0) {
            return 1;
        }
        *(p->in_ptr - 1) = '\0';
        *(p->in_ptr - 2) = '\0';
        // Avoid the other player who wants to steal turns
        if (fd != (game->current_player)->fd) { // The player who typed guess is not the current player
            // Print to server
            int num_read = strlen(p->inbuf) + 2;
            printf("[%d] Read %d bytes\n", fd, num_read);
            printf("[%d] Found newline %c\n", fd, p->inbuf[0]);
            printf("Player %s tried to guess out of turn\n", username);
            // Tell player that they should not guess when it is not the right time
            dp = dprintf(fd, "It's not your turn to guess\r\n");
            if (dp < 0) { // Disconnection
                remove_player(game, &(game->head), fd, "read guess");
            }
            clear_inbuf(p, MAX_BUF);
            return 1;
        }
        // Either the guess is not in lowercase or already guessed or too long for a single letter
        if (p->inbuf[0] < 97 || p->inbuf[0] > 122 || game->letters_guessed[p->inbuf[0] - 97] == 1 || p->inbuf[1] != 0) {
            dp = dprintf(fd, "Please enter a single valid letter\r\n");
            if (dp < 0) { // Disconnection
                remove_player(game, &(game->head), fd, "read guess");
            }
            clear_inbuf(p, MAX_BUF);
            return 1;
        }
        // Nothing is wrong
        return 0;
    }
    // Read not ok
    return 1;
}

// Update the guessed part (from A2)
int update_guessed(struct game_state *game, char *guess) {
    int correct = 0;
    int current_guess_length = strlen(game->word);
    // Loop over the hiding word
    for (int i = 0; i < current_guess_length; i++) {
        // Reveal any guessed hidden letter
        if (game->guess[i] == '-' && game->word[i] == *guess) {
            game->guess[i] = guess[0];
            correct = 1;
        }
    }
    return correct;
}

// Check if the game must end due to no guessing chance left
int no_guess(struct game_state *game) {
    char game_over_msg[MAX_MSG];
    game_over_msg[0] = '\0';
    if (game->guesses_left == 0) {
        // Construct game over message
        strcpy(game_over_msg, "The word was ");
        strcat(game_over_msg, game->word);
        strcat(game_over_msg, "\n");
        strcat(game_over_msg, "No guesses left. Game over.\n");
        strcat(game_over_msg, "\n");
        strcat(game_over_msg, "Let's start a new game\r\n");
        // Broadcast
        broadcast(game, game_over_msg, -1);
        return 1;
    }
    return 0;
}

// Helper for reading from STDIN and writing to socket
int read_username(struct client *p, struct game_state *game, int fd, struct client **new_players) {
    int dp;
    struct client *ptr;

    int nbytes;
    // Receive a name from user. (Code from lab10)
    if ((nbytes = check_read(game, fd, p->in_ptr, MAX_NAME, new_players)) > 0) {
        p->in_ptr += nbytes;
        int where;
        // Any newline character?
        where = find_network_newline(p->in_ptr, MAX_NAME);
        // Avoid partial reads that creates too many messages for user
        if (where == 0) {
            return 1;
        }
        *(p->in_ptr - 1) = '\0';
        *(p->in_ptr - 2) = '\0';
        // Avoid empty input
        int length = strlen(p->inbuf);
        if (length == 0) {
            dp = dprintf(fd, "Please enter a non-empty username\r\n");
            if (dp < 0) { // Disconnection (remove from new player since they can't be in the official game)
                remove_player(game, new_players, fd, "read username");
            }
            clear_inbuf(p, MAX_NAME);
            return 1;
        }
        // Avoid illegal characters
        for (int i = 0; i < length; i++) {
            if (p->inbuf[i] < 32 || p->inbuf[i] > 126) {
                dp = dprintf(fd, "Please enter legal characters\r\n");
                if (dp < 0) { // Disconnection (remove from new player since they can't be in the official game)
                    remove_player(game, new_players, fd, "read username");
                }
                clear_inbuf(p, MAX_NAME);
                return 1;
            }
        }
        // Avoid used names
        for (ptr = game->head; ptr != NULL; ptr = ptr->next) {
            if (strcmp(ptr->name, p->inbuf) == 0) { // The name is already used
                dp = dprintf(fd, "Please enter an username that hasn't been used\r\n");
                if (dp < 0) { // Disconnection (remove from new player since they can't be in the official game)
                    remove_player(game, new_players, fd, "read username");
                }
                clear_inbuf(p, MAX_NAME);
                return 1;
            }
        }
        // Nothing is wrong
        return 0;
    }
    // Read not ok
    return 1;
}


// Removes a new player from un-named linked list
void remove_new_player(struct client **new_players, int fd) {
    struct client **p;

    for (p = new_players; *p && (*p)->fd != fd; p = &(*p)->next)
    ;
    // Now, p points to (1) new players, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d from new players\n", fd);
        // No closing fd for new players since we still want to write in or read from this client
        // No free for the client since its pointer is just moved, not deleted
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n", fd);
    }
}

// Add a client to the head of the official game
void add_new_player(struct client **top, int fd, char *name) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", name);

    p->fd = fd;
    // Import their names
    strcpy(p->name, name);
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}

// Helper for removing a client without closing the socket
void move_to_game(struct client **new_players, int fd, struct game_state *game, char *name) {
    // Remove the user from new players
    struct client *ptr;
    for (ptr = *new_players; ptr != NULL; ptr = ptr->next) {
        if (ptr->fd == fd) { // Found the player to remove
            remove_new_player(new_players, fd);
            break;
        }
    }
    // Add them to game
    if (game->head == NULL) { // There is no active player in game
        add_new_player(&(game->head), fd, name);
        game->current_player = ptr;
        game->current_player = game->head;
    } else { // There are players playing
        add_new_player(&(game->head), fd, name);
    }
}

int main(int argc, char **argv) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;
    
    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }
    
    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int)time(NULL));
    // Set up the file pointer outside of init_game because we want to 
    // just rewind the file when we need to pick a new word
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);
    
    // head and current_player also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.current_player = NULL;
    
    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;
    
    struct sockaddr_in *server = init_server_addr(PORT);
    int listenfd = set_up_server_socket(server, MAX_QUEUE);
    
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            // printf("Connection from %s\n", inet_ntoa(q.sin_addr));
            add_player(&new_players, clientfd, q.sin_addr);
            char *greeting = WELCOME_MSG;
            if(write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                remove_player(&game, &new_players, clientfd, "main");
            };
        }
        
        // To ignore SIGPIPE
        struct sigaction sa;
        sa.sa_handler = SIG_IGN;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        if(sigaction(SIGPIPE, &sa, NULL) == -1) {
            perror("sigaction");
            exit(1);
        }
        
        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be 
         * valid.
         */
        int cur_fd, dp, cmp, exist, num_read, correct, invalid;
        char win_game_msg[MAX_MSG] = {'\0'};
        char game_continue_msg[MAX_MSG] = {'\0'};
        char guess[MAX_BUF] = {'\0'};
        char username[MAX_NAME] = {'\0'};
        for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) {
                // Check if this socket descriptor is an active player
                for(p = game.head; p != NULL; p = p->next) {
                    if (cur_fd == p->fd) {
                        // TODO - handle input from an active client

                        // Read a valid guess from the right person (The first posible line of removing this player entirely)
                        invalid = read_guess(cur_fd, p, &game, p->name);
                        // Check if the player still exists
                        exist = check_exist(&(game.head), cur_fd);
                        if (exist && invalid == 0) { // Not disconnected nor invalid
                            // Copy to guess to make code more readable
                            strcpy(guess, p->inbuf);
                            // Clear it for further reading
                            clear_inbuf(p, MAX_BUF);
                            // Print to server
                            num_read = strlen(guess) + 2;
                            printf("[%d] Read %d bytes\n", cur_fd, num_read);
                            printf("[%d] Found newline %s\n",cur_fd, guess);
                            // Update letter guessed
                            game.letters_guessed[guess[0] - 97] = 1;
                            // Update the word
                            correct = update_guessed(&game, guess);
                            // Compare updated guess with the real word
                            cmp = strcmp(game.guess, game.word);
                            if (cmp == 0) { // The word is guessed out
                                // Construct message for game over
                                strcat(win_game_msg, "The word was ");
                                strcat(win_game_msg, game.word);
                                strcat(win_game_msg, "\r\n");
                                // Broadcast
                                broadcast(&game, win_game_msg, -1);
                                // Announce winner
                                announce_winner(&game, p);
                                // Print to server
                                printf("Game over. %s won!\nNew game\n", p->name);
                                // Restart game
                                init_game(&game, argv[1]);
                                // Announce turn
                                announce_turn(&game);
                                // Print to server
                                printf("It's %s's turn.\n", (game.current_player)->name);
                            } else { // Word is not guessed out
                                // If the guess was wrong
                                if (correct == 0) {
                                    // Tell player not correct
                                    dp = dprintf(cur_fd, "%c is not in the word\r\n", guess[0]);
                                    if (dp < 0) { // Disconnection
                                        remove_player(&game, &(game.head), cur_fd, "main guess wrong");
                                    }
                                    // Game Logic
                                    game.guesses_left -= 1;
                                    advance_turn(&game);
                                    // Print to server
                                    printf("Letter %c is not in the word\n", guess[0]);
                                }
                                // Construct game message since game probably continues
                                strcat(game_continue_msg, p->name);
                                strcat(game_continue_msg, " guesses: ");
                                strncat(game_continue_msg, guess, 1);
                                strcat(game_continue_msg, "\r\n");
                                // Broadcast to everyone
                                broadcast(&game, game_continue_msg, -1);
                                // Construct status message
                                char *turn_msg;
                                if (MAX_GUESSES > 13) { // 14 chances or above will require more space
                                    turn_msg = malloc(2 * MAX_MSG);
                                } else { // 13 chances or below will only require such space
                                    turn_msg = malloc(MAX_MSG);
                                }
                                if (!turn_msg) {
                                    perror("malloc");
                                    exit(1);
                                }
                                turn_msg = status_message(turn_msg, &game);
                                // Broadcast status message
                                broadcast(&game, turn_msg, -1);
                                announce_turn(&game);
                                // Print to server
                                if (!no_guess(&game)) {
                                    printf("It's %s's turn.\n", (game.current_player)->name);
                                }
                                // Free
                                free(turn_msg);
                                // If the game must end due to no guessing chance left
                                if (no_guess(&game)) {
                                    printf("Evaluating for game_over\nNew game\n");
                                    init_game(&game, argv[1]);
                                    // Announce turn
                                    announce_turn(&game);
                                    // Print to server
                                    printf("It's %s's turn.\n", (game.current_player)->name);
                                }
                            }
                            // Must break here
                            break;
                        } else { //Either not exist or not valid
                            break;
                        }
                    }
                }
                // Check if any new players are entering their names
                for(p = new_players; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {
                        // TODO - handle input from an new client who has not entered an acceptable name.
                        // Read a valid username
                        invalid = read_username(p, &game, cur_fd, &new_players);
                        // Check if the user disconnected before enterring a name
                        exist = check_exist(&(new_players), cur_fd);
                        if (exist && invalid == 0) { // The player didn't disconnect and entered a valid name
                            // Copy to username to make code more readable
                            strcpy(username, p->inbuf);
                            // Clear it for further reading
                            clear_inbuf(p, MAX_NAME);
                            // Put the user into official playing game
                            move_to_game(&new_players, cur_fd, &game, username);
                            // Print messages to server
                            num_read = strlen(username) + 2;
                            printf("[%d] Read %d bytes\n", cur_fd, num_read);
                            printf("[%d] Found newline %s\n", cur_fd, username);
                            // Construct joining message
                            char join_msg[MAX_MSG];
                            strcpy(join_msg, username);
                            strcat(join_msg, " has joined.\r\n");
                            // Broadcast to everyone except for who joined
                            broadcast(&game, join_msg, -1);
                            // Printf to server
                            printf("%s", join_msg);
                            printf("It's %s's turn.\n", (game.current_player)->name);
                            // Construct status message
                            char *turn_msg;
                            if (MAX_GUESSES > 13) {  // 14 chances or above will require more space
                                turn_msg = malloc(2 * MAX_MSG);
                            } else {  // 13 chances or below will only require such space
                                turn_msg = malloc(MAX_MSG);
                            }
                            if (turn_msg == NULL) {
                                perror("malloc");
                                exit(1);
                            }
                            turn_msg = status_message(turn_msg, &game);
                            // Let the user know the current game status
                            dp = dprintf(cur_fd, "%s", turn_msg);
                            if (dp < 0) {
                                remove_player(&game, &(game.head), cur_fd, "main add new player");
                            }
                            // Free
                            free(turn_msg);
                            // Announce the new player who should be playing
                            announce_turn(&game);
                            break;
                        } else {
                            break;
                        }
                    }
                }
            }
        }
    }
    return 0;
}
