#include "syscall.h"
#include "string.h"
#include "shell.h"

#define BUFSIZE 1024
#define MAXARGS 8

// pass through to _exec, a file descriptor not used for other things
#define PROGRAM_FD 6

// We need a lot of helper functions through this function
// Create a helper that would skip the leading spaces
// Does this by moving the caller's pointer to the first non whitespace char
// char** allows us to modify the callers pointer
static void get_rid_of_white_space(char **p){

	// Check the character by dereferencing p and check if the characters are equal to an emoty space or a tab
	while(**p == ' ' || **p == '\t'){

		// Iterate the char pointer to the next character in the string
		(*p)++;
	}
}

// Create a helper function that removes the trailing whitespaces from the emd of the string
// Similar to the helper above, the only difference is its at the end and we have to consider newlines
// Overwrites the tab, new line or whitespace characters with \0
static void get_rid_of_trailing_white_space(char *s){

	// Store the number of characters before the first \0, store one more than the last valid character
	int length = strlen(s);

	// The loop will continue as long as the length is greater than 0 and that the last string is one of the whtiespace characters
	while(length > 0 && (s[length - 1] == ' ' || s[length - 1] == '\t' || s[length - 1] == '\n')){

		// Continue setting those characters to "\0"m until loop finishes
		s[--length] = '\0';
	}
}


// helper function for parser
char* find_terminator(char* buf) {
	char* p = buf;
	while(*p) {
		switch(*p) {
			case ' ':
			case '\0':
			case FIN:
			case FOUT:
			case PIPE:
				return p;
			default:
				p++;
				break;
		}
	}
	return p;
}

// Create a helper function that we can call inside of parse
// This function splits a command segment into arguments
// It then detects input redirection and sets *in_path to the filename
// Also detects output redirection and does the same with *outpath
// Modifies the buffer and returns the number of arguemrns in argv
// It basically needs to break it into pieces and recognize < > as input and outpu redirection
// For reference: wc < /file > out
// -> wc is the command, < is the input redirection which means we read from /file, > is the output redirection which means we write to out
static int command_parse(char *buf, char **argv, char **in_path, char **out_path){

	// Initialie the argument counter to 0 as we will count as we go
	int arg_count = 0;
	
	// Create a head character that will walk through the string letter by letter
	char * head = buf;

	// Create a character that marks the end of the word, dont initliaze it as we dont know the end
	char * end;

	// For now, initialize the in_path ond out_path to null as the character hasnt been found
	*in_path = NULL;
	*out_path = NULL;

	// Now create an infinite loop to go through the characters
	for (;;){

		// Skip any spaces before any real charcter by checking for ' ' and '\t'
		while(*head == ' ' || *head == '\t'){

			// Iterate through until it reaches a character
			head++;
		}

		// Check the end of the string condition
		if(*head == '\0'){

			break;
		}

		// Now for the input and output redirection
		// Check for the characters FIN and FOUT
		if(*head == FIN || *head == FOUT){

			// If through the condition, now store what direction it was
			char type = *head;

			// Iterate to the next charaacter
			head++;

			// Now move past the whitespace or not needed chracters
			while(*head == ' ' || *head == '\t'){

				// Iterate through until it reaches a character
				head++;
			}

			// Check the end of the string condition
			if(*head == '\0'){

				break;
			}

			// Now we are past the direction so we need to store the file name
			char *filename = head;

			// Now use the given helper to find the end of the filename
			end = find_terminator(head);

			// Now we need to cut the filename out of the text so that we can move to the next part of the command if there is one
			// First we save it in case there is more
			char save = *end;
			*end = '\0';

			// Now assign the filename to the in or out path
			if(type == FIN){

				*in_path = filename;
			}

			// Otherwise its output
			else{

				*out_path = filename;
			}

			// Now that it was assigned, check if it was the end of the input
			if(save == '\0'){

				break;
			}

			// If not, continue parsing after the filename
			head = end + 1;
			continue;
		}

		// Now we need to handle normal words, if it wasnt < >
		// Normal word so save the start of it
		argv[arg_count++] = head;

		// Check for the max amount of arguments, if too many then stop
		if(arg_count >= MAXARGS){

			// Find the end, set, and break
			end = find_terminator(head);
			*end = '\0';
			break;
		}

		// Now find the end of the arguement and cut it out and save it to isolate and find what it is
		end = find_terminator(head);
		char save = *end;
		*end = '\0';

		// Do the same thing and check if there is more to the command or not
		if(save == '\0'){

			break;
		}

		// Jump to the next word if there is more
		head = end + 1;
	}

	// Set the end of argv[] to null
	argv[arg_count] = NULL;
	return arg_count;
}

int parse(char* buf, char** argv) {
	// FIXME
	// feel free to change this function however you see fit

	// Degine in and out path
	char *in_path;
	char *out_path;

	// call command_parse with the guven parameters
	return command_parse(buf, argv, &in_path, &out_path);


}

// Shell will use _exec to run programs and _exec uses file paths, not just names
// So need to take a command name "trek" and turn it into "c/trek" for the _exec()
static const char *create_exec_path(const char *cmd, char *buf, size_t bufsz){

	// Check if there is a slash in the command name
	// If there isnt, then its ust a simple command name
	// If there is then we know the user already has the path
	if(strchr(cmd, '/') == NULL){

		// Treat as a command name
		snprintf(buf, bufsz, "c/%s", cmd);
		return buf;
	}

	// If has / then use it directly
	return cmd;
}

// Now create a helper for _open() that converts the filenames in the command into real filesystem paths
// Similar thought process to the other path creation for exec but will be needed in run_single
static const char *create_redirection_path(const char *file, char *buf, size_t bufsz){

	// Case 1 where /file becoms c/file
	// Start with the same check, checking if there is a slash as if there is, then we know its the correct path
	if(file[0] == '/'){

		// Build the string but skip the first character, then return buf
		snprintf(buf, bufsz, "c/%s", file + 1);
		return buf;
	}

	// Case 2 where it already starts with c/
	if(file[0] == 'c' && file[1] == '/'){

		// Take the case of c/
		snprintf(buf, bufsz, "%s", file);
		return buf;
	}

	// Case 3 where there is no slash so treat as c/name
	if(strchr(file, '/') == NULL){

		// Treat as c/
		snprintf(buf, bufsz, "c/%s", file);
		return buf;
	}

	// If there is not a /, then we use the filename exactly as how it was typed
	snprintf(buf, bufsz, "%s", file);
	return buf;
}

// Easy helper for error messages
static void print_error(const char *msg){

	// Use the syscall wrapper for the errors so that it goes to CONSOLE_OUT
	if (msg != NULL) {
		_print(msg);
	}
}

// Now we create a helper that runs when the user types a command without pipes (|)
// This function will parse the line into an argv array that holds the commands plus the args
// It will separate the input and output redirection files, will set up the redirections
// It will then call exec to actually run the program
static void run_single(char *cmd_line){

	// Initialize the in and out paths as NUL as they wull hold the filenames for < and > if present
	char *in_path = NULL;
	char *out_path = NULL;

	// Initialize the argument array where argv[0] will be the command name and from there on are the arguments
	// Initialize to one more than the max as the last one is reserved for a NULL terminator
	char *argv[MAXARGS + 1];

	// Create two temp buffers for the exec path an the redirection path
	char exec_buf[64];
	char path_buf[64];

	// Add file descriptor variables for the redirections
	int input_fd = STDIN;
	int output_fd = STDOUT;

	// Remove the trailing whitespace from the end of the string using our helper
	get_rid_of_trailing_white_space(cmd_line);

	// Crreate a new pointe taht starts at the command line so that we can move forward without modifying the input
	char *segment = cmd_line;

	// Now move the pointer forward until the first useful character using the helper
	get_rid_of_white_space(&segment);

	// Check if its now empty by checking if segment is the same as '/0'
	if(*segment == '\0'){

		return;
	}

	// Now use the parse helper we made to parse the command line into arguments and redirection
	int argc = command_parse(segment, argv, &in_path, &out_path);

	// If nothing was found then exit, this is the check for that
	if(argc == 0){

		return;
	}

	// Now use the exec path helper to create the executable path
	const char *exec_path = create_exec_path(argv[0], exec_buf, sizeof(exec_buf));

	// Now exec_path has the path and we can overwrite argv[0] with aht so that when we call exec, the first arg is the full path to the program
	argv[0] = (char *)exec_path;

	// DEBUG: pre-open the redirection files in the parent
	// Move the code from inside the process id loop to outside so its open before
	if(in_path){

		// First we use the helper to conver the users filename into a real path
		const char *p = create_redirection_path(in_path, path_buf, sizeof(path_buf));

		// Open the inpuit file using an unused fd
		input_fd = _open(3, p);
			
		// Now we open the path p onto file decriptor 0 and if it fails, send a message to the console
		if(input_fd < 0){

			print_error("input redirection failure\n");
			return;
		}

	}

	// Now we do the same thing for the output redirection, very similar
	if(out_path){

		// First we use the helper to conver the users filename into a real path
		const char *p = create_redirection_path(out_path, path_buf, sizeof(path_buf));

		// Now we alos need to create a file at path p using file create as its an output file
		(void)_fscreate((char *) p);

		// Create the output and open on the spare fd
		output_fd = _open(4, p);

		// Now we open the path p onto file decriptor 1 (STDOUT) and if it fails, send a message to the console
		if(output_fd < 0){

			print_error("output redirection failure\n");
			
			// Check if the input fd is not equal to the STDIN, then close it
			if(input_fd != STDIN){

				_close(input_fd);
			}

			return;
		}

	}


	// Before touching any of the file descriptors, need to fork to keep the shell running
	// Use _fork to duplicate the current process 
	int process_id = _fork();

	// Check to make sure it forked correctly
	// If it is less than 0, then it has failed
	if(process_id < 0){

		print_error("fork failed\n");
		return;
	}

	// Now if its the child
	if(process_id == 0){

		// Now we need to handle the two redirections, starting with the input
		// We only go through this case if the in_path is not NULL meaning there was a < somewhere in the cmd line
		// if(in_path){

		// 	// First we use the helper to conver the users filename into a real path
		// 	const char *p = create_redirection_path(in_path, path_buf, sizeof(path_buf));

		// 	// DEBUG
		// 	_print("REDIR IN open: ");
		// 	_print(p);
		// 	_print("\n");

		// 	// Now we need to close STDIN, which is file descriptor 0 so that when open is called, i t refers to the file instead of the console
		// 	_close(STDIN);
			
		// 	// Now we open the path p onto file decriptor 0 and if it fails, send a message to the console
		// 	if(_open(STDIN, p) < 0){

		// 		print_error("input redirection failure\n");
		// 		_exit();
		// 	}

		// }

		// // Now we do the same thing for the output redirection, very similar
		// if(out_path){

		// 	// First we use the helper to conver the users filename into a real path
		// 	const char *p = create_redirection_path(out_path, path_buf, sizeof(path_buf));

		// 	// DEBUG
		// 	_print("REDIR OUT create: ");
		// 	_print(p);
		// 	_print("\n");

		// 	// Now we alos need to create a file at path p using file create as its an output file
		// 	(void)_fscreate((char *) p);

		// 	// Now we need to close STDOUT, which is file descriptor 1 so that when open is called, i t refers to the file instead of the console
		// 	_close(STDOUT);

		// 	// DEBUG
		// 	_print("REDIR OUT open: ");
		// 	_print(p);
		// 	_print("\n");
			
		// 	// Now we open the path p onto file decriptor 1 (STDOUT) and if it fails, send a message to the console
		// 	if(_open(STDOUT, p) < 0){

		// 		print_error("output redirection failure\n");
		// 		_exit();
		// 	}

		// }

		// Replace that entire section with duplicating code so that it remaps the already open descriptors with uiodup
		if(input_fd != STDIN){

			// close STDIN
			_close(STDIN);

			// Use uiodup to duplicate into STD_IN
			_uiodup(input_fd, STDIN);

			// Now we can close the input fd
			_close(input_fd);
		}

		// Same thought process for output
		if(output_fd != STDOUT){

			// close STDOUT
			_close(STDOUT);

			// Use uiodup to duplicate into STD_OUT
			_uiodup(output_fd, STDOUT);

			// Now we can close the output fd
			_close(output_fd);
		}

		// Open the program so that it is ready to be used in exec
		int fd = _open(PROGRAM_FD, exec_path);
		
		// Check for failure
		if(fd < 0){

			print_error("failed to open program\n");
			_exit();
		}

		// Now we execute the program using _exec
		int exec_run = _exec(fd, argc, argv);

		// Create an error check in case it failed
		if(exec_run < 0){

			print_error("_exec of program failed\n");
		}

		_exit();
	}

	// Now we have to close the redirection fds in the parent after the fork
    if(process_id > 0){

		// As we opened, close them so
        if(input_fd != STDIN){

            _close(input_fd);
        }
        if(output_fd != STDOUT){

            _close(output_fd);
        }
    }
	
	// Now the parent will wait
	_wait(process_id);

	// DEBUG
	//printf("back in shell after child %d\n", process_id);

}

// Now we need to create a helper that deals with pipes
// If we have a line like left | right, we need to run the two programs at the same time
// We also need to connect them so that the output of the left program is fed into the input of the right
// Create run pipeline to have this happen
// Create a pipe, fork the two child processes, in each child call exec, then parent shell waits for it to finish
static void run_pipeline(char *left, char *right){

	// Allocate space for the arguments on the left and right side
	char *argv_left[MAXARGS + 1];
	char *argv_right[MAXARGS + 1];

	// Now create the filenames for the input and output of each file, initialize to null for now
	char *in_path_left = NULL;
	char *out_path_left = NULL;
	char *in_path_right = NULL;
	char *out_path_right = NULL;

	// Create the temporary execute buffers for each side but there is only one path buffer needed
	char exec_buf_left[64], exec_buf_right[64];
	char path_buf[64];

	// File descriptor for left side input redirection
	int left_input_fd = STDIN;

	// Now we need to clean up the command by getting rid of whitespace in the beginning and in the end of each command
	get_rid_of_trailing_white_space(left);
	get_rid_of_trailing_white_space(right);
	get_rid_of_white_space(&left);
	get_rid_of_white_space(&right);

	// Check if either side ends up empty, if they do then return
	if(*left == '\0' || *right == '\0'){

		return;
	}

	// Now parse the two sides and store the number of arguments for each cusing command_parse
	int argc_left = command_parse(left, argv_left, &in_path_left, &out_path_left);
	int argc_right = command_parse(right, argv_right, &in_path_right, &out_path_right);

	// Check if either of the arg counters are 0, if they are then return
	if(argc_left == 0 || argc_right == 0){

		return;
	}

	// No need to do redirection on the left side as we dont need to
	// So if there is an output to the left input pathm then return
	if(out_path_left){

		print_error("output redirection on the left side is not in the code\n");
		return;
	}

	if(in_path_right){

		print_error("input redirecton on the right side is not in the code\n");
		return;
	}

	// Pre open the left input redirection:
	// Handle input redirection on the left
	// Now we need to handle the input redirection as we did in the past
	if(in_path_left){

		// First we use the helper to conver the users filename into a real path
		const char *p = create_redirection_path(in_path_left, path_buf, sizeof(path_buf));

		// Open the input file on an unused fd
		left_input_fd = _open(3, p);
			
		// If this fails, print error and exit
		if(left_input_fd < 0){

			print_error("left input redirection failure\n");
			return;
		}
	}

	// Now we can decide which programs to execute, similar to run a single line, create the path to run
	// Overwrite the first arguemtn int argv which is the command with this path so it knows what to run
	const char *exec_left = create_exec_path(argv_left[0], exec_buf_left, sizeof(exec_buf_left));
	argv_left[0] = (char *)exec_left;

	// Do the same for the right
	const char *exec_right = create_exec_path(argv_right[0], exec_buf_right, sizeof(exec_buf_right));
	argv_right[0] = (char *)exec_right;

	// Now we need to create the actual pipe
	// Define the file descriptors for reading and writing into the pipe;
	// Initialize to -1 so that the kernel knows they are invalid or not set
	int write_fd = -1;
	int read_fd = -1;

	// Now we can call upon the pipe function so that passing &write_fd means we can etie into write_fd
	// _pipe function returns 0 or a positive value if succesful so <0 means it has failed
	if(_pipe(&write_fd, &read_fd) < 0){

		print_error("pipe failed\n");
		return;
	}

	// Now we do the first fork, the left side
	// Use _fork to duplicate the current process 
	int process_left_id = _fork();

	// Check to make sure it forked correctly
	// If it is less than 0, then it has failed
	if(process_left_id < 0){

		print_error("fork failed\n");
		return;
	}

	// If the fork was succesful, one process sees the id > 0 which is parent and the other is 0 which is the child
	// Now check if the process_left is 0, that means that its the child path
	if(process_left_id == 0){

		// Now we check if there was input redirection on the left
		if(left_input_fd != STDIN){

			// Close the current STDIN
			_close(STDIN);
			
			// Duplicate the file onto STDIN
			_uiodup(left_input_fd, STDIN);

			// No longer need it now so close
			_close(left_input_fd);
		}

		// We want the STDOUT to go the pipe write end so redirect it to that
		_close(STDOUT);
		_uiodup(write_fd, STDOUT);

		// Now STDOUT points to the pipe write end and any output goes into the pipe
		// Now we close the read and write end in the left side
		_close(write_fd);
		_close(read_fd);


		// Open the program so that it is ready to be used in exec
		int fd = _open(PROGRAM_FD, exec_left);
		
		// Check for failure
		if(fd < 0){

			print_error("failed to open left program\n");
			_exit();
		}

		// Now we can execute the first command as we did before
		int exec_run = _exec(fd, argc_left, argv_left);

		// Create an error check in case it failed
		if(exec_run < 0){

			print_error("_exec of left program failed\n");
		}

		// If exec fails, then exit
		_exit();
	}

	// If after left fork the left_input_fd was used, close it
	if(process_left_id > 0 && left_input_fd != STDIN){

		_close(left_input_fd);
	}

	// Now for the right side, very similar start
	// Use _fork to duplicate the current process 
	int process_right_id = _fork();

	// Check to make sure it forked correctly
	// If it is less than 0, then it has failed
	if(process_right_id < 0){

		print_error("second fork failed\n");
		return;
	}
	
	// Now we do the same thing, except this time we need to handle input and output redirection if necessary
	if(process_right_id == 0){

		// We want the STDIN to go the pipe read end so redirect it to that
		_close(STDIN);
		_uiodup(read_fd, STDIN);

		// Now STDIN points to the pipe read end and any output goes into the pipe
		// Only runs into the right child si the STDIN goes to the pipes read end as we wanted
		_close(write_fd);
		_close(read_fd);

		// Now we need to handle the inpute redirection as we did in the past
		// if(in_path_right){

		// 	// First we use the helper to conver the users filename into a real path
		// 	const char *p = create_redirection_path(in_path_right, path_buf, sizeof(path_buf));

		// 	// Now we need to close STDIN, which is file descriptor 0 so that when open is called, i t refers to the file instead of the console
		// 	_close(STDIN);
			
		// 	// Now we open the path p onto file decriptor 0 and if it fails, send a message to the console
		// 	if(_open(STDIN, p) < 0){

		// 		print_error("right input redirection failure\n");
		// 		_exit();
		// 	}
		// }

		// Follow the same process for out with create
		if(out_path_right){

			// First we use the helper to conver the users filename into a real path
			const char *p = create_redirection_path(out_path_right, path_buf, sizeof(path_buf));

			// Now we alos need to create a file at path p using file create as its an output file
			(void)_fscreate((char *) p);

			// Now we need to close STDOUT, which is file descriptor 1 so that when open is called, i t refers to the file instead of the console
			_close(STDOUT);
			
			// Now we open the path p onto file decriptor 1 (STDOUT) and if it fails, send a message to the console
			if(_open(STDOUT, p) < 0){

				print_error("right output redirection failure\n");
				_exit();
			}
		}

		// Now execute this program as we did before

		// Open the program so that it is ready to be used in exec
		int fd = _open(PROGRAM_FD, exec_right);
		
		// Check for failure
		if(fd < 0){

			print_error("failed to open right program\n");
			_exit();
		}
		// Now we can execute the first command as we did before
		int exec_run = _exec(fd, argc_right, argv_right);

		// Create an error check in case it failed
		if(exec_run < 0){

			print_error("_exec of right program failed\n");
		}

		// If exec fails, then exit
		_exit();
	}

	// Now the parent closees the pipe file descriptors and waits for the children
	_close(write_fd);
	_close(read_fd);

	// Wait for the children
	_wait(process_left_id);
	_wait(process_right_id);
}

int main()
{
    char buf[BUFSIZE];
	//int argc;
	//char* argv[MAXARGS + 1]; 

  	_open(CONSOLEOUT, "dev/uart1");		// console device
	_close(STDIN);              		// close any existing stdin
	_uiodup(CONSOLEOUT, STDIN);      	// stdin from console
	_close(STDOUT);              		// close any existing stdout
	_uiodup(CONSOLEOUT, STDOUT);     	// stdout to console

	printf("Starting 391 Shell\n");

	for (;;)
	{
		printf("LUMON OS> ");
		getsn(buf, BUFSIZE - 1);

		// Get rid of trailing white spaces first
		get_rid_of_trailing_white_space(buf);

		// We first need to get rid of the leading spaces
		char *cmd = buf;
		get_rid_of_white_space(&cmd);
		
		// Now wee need to ignore any empty input
		if(*cmd == '\0'){

			continue;
		}

		if (0 == strcmp(cmd, "exit")){

			_exit();
		}

		// Now we need to detect if it is a pipe or not
		// First assume there is no pipe
		char * pipe_position = NULL;

		// Now loop through the characters in the cmd looking for the pipe command |
		for(char *p = cmd; *p; p++){

			// If found, update the position of the pipe and break
			if(*p == PIPE){
	
				pipe_position = p;
				break;
			}
		}

		// Now we can deal with the pipe, and if its not we just run the line
		// If its not NULL
		if(pipe_position){

			// Replace the | with the end character so that there are techinally now two strings
			*pipe_position = '\0';

			// Now we can differentiate the left and right commands
			// The left command will start at cmd
			char *left = cmd;

			// The right command will start at the position of the pipe + 1
			char *right = pipe_position + 1;

			// Now we can call run_pipeline as this will take care of it
			run_pipeline(left, right);
		}

		// If not a pipe, then run a single line using the helper
		else{

			run_single(cmd);
		}

	}
}