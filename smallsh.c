#define _POSIX_C_SOURCE 200809L			// define POSIX feature macro for sigaction

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <signal.h>
#include <fcntl.h>


/**
 * Foreground Only Flag
 *  Volatile forces shell to read the current state from memory instead of cached CPU value (prevents missed update).
 * 	sig_atomic_t is an int type that guarantees that reading and writing happen as single uninterrupted operation (prevents corrupted half-written state).
*/ 
volatile sig_atomic_t fgOnly = 0;

/**
 * Struct representing a command
 */
struct command {
	char* args[512];
	char* inputFile;
	char* outputFile;
	bool background;
	int argCount;
};

/**
 * Forward Declarations
 */
void parseCommand(char* cmdline, struct command* cmd);
void freeCommand(struct command* cmd);
void changeDir(struct command* cmd);
void printStatus(int status);
void executeCommand(struct command* cmd);
void cleanBackground(pid_t* bgPids, int pidCount);
void childReaper(pid_t* bgPids, int* pidCount);
void handle_SIGTSTP(int signo);

int main(void){
	/**
	 * SIGNAL HANDLERS
	 */
	struct sigaction SIGINT_action = {0}, SIGTSTP_action = {0};

	/**
	 * SIGINT (ctrl+c)
	 * 	SIGINT is set to be ignored by shell and background children, 
	 *  foreground children will override with SIGINT_action.sa_handler = SIG_DFL
	 */
	SIGINT_action.sa_handler = SIG_IGN;
	sigfillset(&SIGINT_action.sa_mask);					
	SIGINT_action.sa_flags = 0;
	sigaction(SIGINT, &SIGINT_action, NULL);

	/**
	 * SIGTSTP (ctrl+z)
	 *  Shell catches SIGTSTP and toggles foreground mode
	 */
	SIGTSTP_action.sa_handler = handle_SIGTSTP;
	sigfillset(&SIGTSTP_action.sa_mask);				// prevents handler from being interrupted by other signals
	SIGTSTP_action.sa_flags = SA_RESTART;				// prevents interrupt on fgets() prompt read
	sigaction(SIGTSTP, &SIGTSTP_action, NULL);

	bool running = true;
	int childStatus = 0;

	/**
	 * Dynamically allocate PID array
	 *	If pidCount == capacity when adding a PID, we double capacity realloc() more space.
	 *	This is likely entirely unecessary, but it's fun so why not? Memory is cleaned up using
	 * 	cleanBackground()
	 */
	int capacity = 16;
	int pidCount = 0;
	
	// TODO: error handler
	pid_t* bgPids = malloc(capacity * sizeof(pid_t));

	do{	
		char cmdline[2048];

		// Reap zombie children before ":" prompt
		childReaper(bgPids, &pidCount);

		// Prompt, flush buffer, and store input.
		printf(": ");
		fflush(stdout);
		fgets(cmdline, 2048, stdin);
		// !replace stored input "\n" with null terminator
		// PROBABLY SHOULD CHECK FOR NULL BEFORE THIS
		cmdline[strcspn(cmdline, "\n")] = '\0';

		// Comment or blank line dectection 
		if(cmdline[0] == '#' || strlen(cmdline) == 0){
			continue;
		}

		// Allocate mem for command and parse input
		struct command* cmd = calloc(1, sizeof(struct command));
		parseCommand(cmdline, cmd);
		
		/**
		 * Command Handler
		 *  Standards commands are handled using the relevant functions.
		 * 	Non-standard commands are forked, and the child calls executeCommand(),
		 * 	and parent determines if this is a background or foreground command. If
		 * 	bg add child's PID bgPids array and continue (we reap bg children before
		 *  prompting ":"), otherwise call waitpid now().
		 */
		if(strcmp(cmd->args[0], "exit") == 0){
			running = false;
			freeCommand(cmd);
			continue;
		} else if(strcmp(cmd->args[0], "cd") == 0) {
			changeDir(cmd);
		} else if(strcmp(cmd->args[0], "status") == 0){
			printStatus(childStatus);
		} else{
			pid_t childPid = fork();

			if(childPid == -1){
				// Fork error handler
				perror("fork");
				exit(EXIT_FAILURE);
			} else if(childPid == 0){
				// CHILD SEGMENT

				// Children ignore SIGTSTP
				SIGTSTP_action.sa_handler = SIG_IGN;
				sigaction(SIGTSTP, &SIGTSTP_action, NULL);

				if(!cmd->background){
					// Override SIGINT action with default behavior on foreground child
					SIGINT_action.sa_handler = SIG_DFL;
					sigaction(SIGINT, &SIGINT_action, NULL);
				}

				executeCommand(cmd);
			} else {
				// PARENT SEGMENT, non-blocking wait pass ref to childstatus
				if(cmd->background){
					printf("background pid is %d\n", childPid);
					// Realloc more space if PID array is full
					if(pidCount == capacity) {
						capacity *= 2;
						bgPids = realloc(bgPids, capacity * sizeof(pid_t));
					}
					// Add childPid to PID array
					bgPids[pidCount++] = childPid;
				} else{
					childPid = waitpid(childPid, &childStatus, 0);
					if(WIFSIGNALED(childStatus)){
						printStatus(childStatus);
					}
				}
			}
		}
		freeCommand(cmd);		
	} while(running);
	// Free background PID array memory before exiting
	// TODO: Possibly killing reused PIDS? Implement handle for that
	cleanBackground(bgPids, pidCount);
	return 0;
}

/**
 * parseCommand
 * 	Parses command entered into an array and returns using struct command* cmd
 */
void parseCommand(char* cmdline, struct command* cmd){
	char* token = strtok(cmdline, " ");
	char* lastToken = NULL;

	/**
	 * Flags for detecting input or ouput
	 * 	If we detect a '<' or '>', we know the next token is an input or output file respectively.
	 */ 
	int inputExpected = 0;
	int outputExpected = 0;

	// Argument counter
	int i = 0;

	// Token string comparison
	while(token != NULL){
		if(inputExpected){
			// PrevToken == '<'
			cmd->inputFile = strdup(token);
			// Reset flag
			inputExpected = 0;
		} else if(outputExpected){
			// PrevToken == '>'
			cmd->outputFile = strdup(token);
			outputExpected = 0;
		} else if(strcmp(token, "<") == 0){
			// Set input flag
			inputExpected = 1;
		} else if(strcmp(token, ">") == 0){
			// Set output flag
			outputExpected = 1;
		} else{
			// Add argument to array
			cmd->args[i] = strdup(token);
			i++;
		}
		lastToken = token;
		token = strtok(NULL, " ");

		/**
		 * Background flag detection
		 * 	Using lastToken to track the previous token, we strcmp() the final non-null token with '&'.
		 *  This avoids accidentally flagging any '&' within commandline as a background flag.
		 * 	TODO: Awkward freeing and removal of final arg, rewrite parser to peek later.
		 */
		if(token == NULL){
			if(strcmp(lastToken, "&") == 0){
				free(cmd->args[i-1]);
				cmd->args[i-1] = NULL;
				i--;
				if(!fgOnly){
					cmd->background = true;
				}
			} 
		}
	}
	// Null terminate args for execvp()
	cmd->args[i] = NULL;
	cmd->argCount = i;
}

/**
 * freeCommand
 * 	Free properties and command struct allocated on heap using strdup()
 */
void freeCommand(struct command* cmd){
	for(int i = 0; i < cmd->argCount; i++){
		free(cmd->args[i]);
	}
	if(cmd->inputFile)
		free(cmd->inputFile);
	if(cmd->outputFile)
		free(cmd->outputFile);
	free(cmd);
}

/**
 * changeDir
 * 	Change working directory to environment HOME variable if cmd = "cd"
 * 	Change working directory to args[1] otherwise.
 */
void changeDir(struct command* cmd){
	if(cmd->argCount == 1){
		chdir(getenv("HOME"));
	} else {
		int result = chdir(cmd->args[1]);
		if(result == -1){
			perror("cd");
		}
	}
}

/**
 * printStatus
 * 	print childStatus exit value
 *  print Signal status
 */
void printStatus(int status){
	if(WIFEXITED(status)){
		printf("exit value %d\n", WEXITSTATUS(status));
	} else if(WIFSIGNALED(status)){
		printf("terminated by signal %d\n", WTERMSIG(status));
	}
}

/**
 * executeCommand
 *  Execute other commands incl. I/O Redirection
 */
void executeCommand(struct command* cmd){
	int result = 0;		// Stores dup2() return value. Non-neg = Succes, -1 = Error

	/**
	 * Background i/o redirect
	 * 	Opens file descriptor and then redirect stream w/ dup2.
	 * 	Closes the fd and then error checks the dup2's result. 
	 */ 
	if(cmd->background){
		if(cmd->inputFile == NULL){
			// Redirect stdin to /dev/null
			int devNull = open("/dev/null", O_RDONLY);
			result = dup2(devNull, 0);
			close(devNull);
			if(result == -1){
				perror("/dev/null in dup2()");
				exit(1);
			}
		}
		if(cmd->outputFile == NULL){
			// Redirect stdout to /dev/null
			int devNull = open("/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
			result = dup2(devNull, 1);
			close(devNull);
			if(result == -1){
				perror("/dev/null out dup2()");
				exit(1);
			}	
		}
	}

	/**
	 * Non-background i/o redirect
	 */
	if(cmd->inputFile){
		int sourceFD = open(cmd->inputFile, O_RDONLY);
		if(sourceFD == -1){
			fprintf(stderr, "cannot open %s for input\n", cmd->inputFile);
			exit(1);
		}

		// Redirect stdin to source file
		result = dup2(sourceFD, 0);
		close(sourceFD);
		if(result == -1) {
			perror("source dup2()");
			exit(1);
		}
}

	if(cmd->outputFile){
		// Open target file
		int targetFD = open(cmd->outputFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if(targetFD == -1){
			fprintf(stderr, "cannot open %s for output\n", cmd->outputFile);
			exit(1);
		}

		// Redireect stdout to target file
		result = dup2(targetFD, 1);
		close(targetFD);
		if(result == -1){
			perror("target dup2()");
			exit(1);
		}
	}

	/**
	 * Non-standard command Handler
	 * 	Passes arg[0] from commandline and args array
	 */
	execvp(cmd->args[0], cmd->args);

	// !NOTE: POTENTIALLY CASE SENSITIVE ON TURNITIN? Use fprintf if issue
	perror(cmd->args[0]);
	exit(EXIT_FAILURE);
}

/**
 * cleanBackground
 * 	Iterates through background PID array, terminating processes.
 *  Frees dynamically allocated array after cleanup.
 */
void cleanBackground(pid_t* bgPids, int pidCount){
	for(int i = 0; i < pidCount; i++){
		kill(bgPids[i], SIGTERM);
	}
	free(bgPids);
}

/**
 * childReaper
 * 	Check PID array to see if any children are ready for reaping.
 */
void childReaper(pid_t* bgPids, int* pidCount){
	int status = 0;
	for(int i = 0; i < *pidCount; i++){
		pid_t childPid = waitpid(bgPids[i], &status, WNOHANG);

		if(childPid > 0){
			// Output status if reaped
			if(WIFEXITED(status)){
				printf("background pid %d is done: exit value %d\n", bgPids[i], WEXITSTATUS(status));
			} else if(WIFSIGNALED(status)){
				printf("background pid %d is done: terminated by signal %d\n", bgPids[i], WTERMSIG(status));
			}
			
			/**
			 * Overwrite reaped PID with last element and decrement count
			 */
			bgPids[i] = bgPids[--(*pidCount)];
			i--;
		}
	}
}

/**
 * handler_SIGTSTP
 * 	Checks foreground only flag and sets it appropriately and outputs new mode to shell.
 */
void handle_SIGTSTP(int signo){
	char* message;
	if(fgOnly){
		fgOnly = 0;
		message = "\nExiting foreground-only mode\n: ";
	} else{
		fgOnly = 1;
		message = "\nEntering foreground-only mode (& is now ignored)\n: ";
	}
	write(STDOUT_FILENO, message, strlen(message));
}