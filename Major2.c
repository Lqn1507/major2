#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>


#define MAX 512 //user's input is less than 512 bytes
#define BUFFER_SIZE 1024
#define ALIAS_USAGE \
    "Usage of alias:\n" \
    "alias                      - Display a list of all aliases\n" \
    "alias alias_name='command' - Add a new alias\n" \
    "alias -r alias_name        - Remove a single alias\n" \
    "alias -c                   - Remove all aliases" \

int hist_count = 0; //global variable for MyHistory function
pid_t ppid; //gloabal parent id
pid_t cpid; //global child id

void InteractiveMode();
void BatchMode(char *file);

int ParseCommands(char *userInput); //e.g., "ls -a -l; who; date;" is converted to "ls -al" "who" "date"
int ParseArgs(char *full_line, char *args[]); //e.g., "ls -a -l" is converted to "ls" "-a" "-l"
void ExecuteCommands(char *command, char *full_line);

void MyCD(int argc, char* argv[]); void MyExit();
void MyPath(char *args[], int arg_count);
void MyHistory(char *args[], int arg_count);

void CommandRedirect(char *args[], char *first_command, int arg_count, char *full_line);
void PipeCommands(char *args[], char *first_command, int arg_count);
void signalHandle(int sig);
void io_redirect(char *command, char *full_line);

char CURRENT_DIRECTORY[MAX]; //current directory
char *COMMANDS[MAX]; //commands to be executed
char *MYHISTORY[MAX]; //shell command history
char *MYPATH; //my PATH variable
const char *ORIG_PATH_VAR; //The original PATH contents
char *prompt;

int EXIT_CALLED = 0;//Functions seem to treat this as a global variable -DM
typedef struct Alias alias_t;
typedef struct Alias* alias_ptr_t;

// struct to store lise of alias commands
struct Alias
{
    char* name;
    char* command;
    alias_ptr_t next;
};
alias_ptr_t alias_ptr=NULL;
// returns a pointer to a heap allocated alias struct
// create a alias command
alias_ptr_t alias_create(char* name, char* command)
{
    alias_ptr_t new_alias_ptr = (alias_ptr_t) malloc(sizeof(alias_t));

    *new_alias_ptr =
        (alias_t) {
            .name = name,
            .command = command,
            .next = NULL
        };

    return new_alias_ptr;
}

// delete particular alias command
void alias_free(alias_ptr_t alias_ptr)
{
    if(alias_ptr != NULL)
    {
        free(alias_ptr->name);
        free(alias_ptr->command);

        *alias_ptr =
            (alias_t) {
                .name = NULL,
                .command = NULL,
                .next = NULL
            };

        free(alias_ptr);
    }
}

// recursively frees the entire alias data structure with the
// passed argument as the head of the list
alias_ptr_t alias_destroy(alias_ptr_t alias_ptr)
{
    if(alias_ptr)
    {
        alias_destroy(alias_ptr->next);
        alias_free(alias_ptr);
    }

    return NULL;
}

// removes elements from the alias list that have a matching name
// and returns the new head of the list
alias_ptr_t alias_remove(alias_ptr_t alias_ptr, const char* name)
{
    if(alias_ptr == NULL)
    {
        // empty list
        return NULL;
    }

    if(strcmp(alias_ptr->name, name) == 0)
    {
        // first element matches
        alias_ptr_t next = alias_ptr->next;
        alias_free(alias_ptr);
        return next;
    }

    // removing from the middle to the end of the list
    alias_ptr_t iterator = alias_ptr;

    while(iterator->next != NULL)
    {
        if(strcmp(iterator->next->name, name) == 0)
        {
            alias_ptr_t next = iterator->next->next;
            alias_free(iterator->next);
            iterator->next = next;
            break;
        }

        iterator = iterator->next;
    }

    return alias_ptr;
}

// adds a new (name, command) pair to the list of aliases
// by first removing any element with the passed name and
// returns the new head of the list of aliases
alias_ptr_t alias_add(alias_ptr_t alias_ptr, const char* name, const char* command)
{
    alias_ptr = alias_remove(alias_ptr, name);

    alias_ptr_t new_alias_ptr = alias_create(strdup(name), strdup(command));

    if(alias_ptr == NULL)
    {
        // list was empty
        return new_alias_ptr;
    }

    // going to the end of the alias list
    alias_ptr_t end_ptr = alias_ptr;
    while(end_ptr->next)
        end_ptr = end_ptr->next;

    // adding new_alias_ptr to the end
    end_ptr->next = new_alias_ptr;

    return alias_ptr;
}

// recursively displays the entire alias data structure
void alias_display(const alias_ptr_t alias_ptr)
{
    if(alias_ptr)
    {
        printf("%s=\"%s\"\n", alias_ptr->name, alias_ptr->command);
        alias_display(alias_ptr->next);
    }
}

// recursively searches for a command with the passed name
// in the entire alias data structure and returns NULL if
// not found
char* alias_query(const alias_ptr_t alias_ptr, const char* name)
{
    if(alias_ptr == NULL)
    {
        // not found
        return NULL;
    }

    if(strcmp(alias_ptr->name, name) == 0)
    {
        // match found
        return alias_ptr->command;
    }

    // search in the rest
    return alias_query(alias_ptr->next, name);
}
// executes non-alias-prefixed commands using system
bool execute_other_command(char* command, const alias_ptr_t alias_ptr)
{
    char* query = alias_query(alias_ptr, command);

    if(query != NULL)
    {
        command = query;
	system(command);
	return true;
    }
	return false;
}


// executes alias commands
alias_ptr_t execute_alias_command(char* command, alias_ptr_t alias_ptr)
{
    bool incorrect_usage = false;

    if(strncmp(command, "alias -", strlen("alias -")) == 0)
    {
        // alias with options
        if(command[7] == 'c')
        {
            // remove all aliases
            alias_ptr = alias_destroy(alias_ptr);
        }
        else if(command[7] == 'r')
        {
            // remove a single alias
            char alias_name[BUFFER_SIZE];
            sscanf(command, "alias -r %s", alias_name);
            alias_ptr = alias_remove(alias_ptr, alias_name);
        }
        else
        {
            incorrect_usage = true;
        }
    }
    else if(command[5] != '\0')
    {
        // set alias
        char* alias_name;
        char* alias_command;

        // start of alias name
        alias_name = command + 6;

        // finding assignment operator
        char* iterator = alias_name + 1;
        bool assignment_found = false;
        while(*iterator != '\0')
        {
            if(*iterator == '=')
            {
                assignment_found = true;
                break;
            }

            ++iterator;
        }

        if(assignment_found)
        {
            *iterator = '\0'; // replacing assignment operator with '\0'
            ++iterator;

            // quote at start of alias command
            if(*iterator == '\'')
            {
                alias_command = ++iterator;

                // finding ending quote after alias command
                bool quote_found = false;
                while(*iterator != '\0')
                {
                    if(*iterator == '\'')
                    {
                        quote_found = true;
                        break;
                    }

                    ++iterator;
                }

                if(quote_found)
                {
                    *iterator = '\0'; // replacing quote with '\0'

                    // adding alias
                    alias_ptr = alias_add(alias_ptr, alias_name, alias_command);
                }
                else
                {
                    incorrect_usage = true;
                }
            }
            else
            {
                incorrect_usage = true;
            }
        }
        else
        {
            incorrect_usage = true;
        }
    }
    else
    {
        // display aliases
        alias_display(alias_ptr);
    }

    if(incorrect_usage)
    {
        // incorrect usage
        puts("Incorrect usage.");
        puts(ALIAS_USAGE);
    }

    return alias_ptr;
}


int main(int argc, char *argv[]){
    //error checking on user's input
	  if (!(argc < 3)) {
		  fprintf(stderr, "Error: Too many parameters\n");
		  fprintf(stderr, "Usage: './output [filepath]'\n");
		  exit(0);//No memory needs to be cleared
	  }
    //initialize your shell's enviroment
    MYPATH = (char*) malloc(1024);
	  memset(MYPATH, '\0', sizeof(MYPATH));
	  ORIG_PATH_VAR = getenv("PATH"); // needs to include <stdlib.h>

    //save the original PATH, which is recovered on exit
	  strcpy(MYPATH, ORIG_PATH_VAR);

    //make my own PATH, namely MYPATH
	  setenv("MYPATH", MYPATH, 1);

	  if(argc == 1) InteractiveMode();
     else if(argc == 2) BatchMode(argv[1]);

		//gets the parent id and sets it to ppid
    ppid = getpid();

    //handles the signal (Ctrl + C)
    signal(SIGINT, signalHandle);

    //handles the signal (Ctrl + Z)
    signal(SIGTSTP, signalHandle);

    //free all variables initialized by malloc()
	free(MYPATH);
    setenv("PATH", ORIG_PATH_VAR, 1);
	alias_ptr = alias_destroy(alias_ptr);
	return 0;
}

void BatchMode(char *file){

	FILE *fptr = fopen(file, "r");
    //error checking for fopen function
    if(fptr == NULL) {
		fprintf(stderr, "Error: Batch file not found or cannot be opened\n");
		MyExit();
    }

    char *batch_command_line = (char *)malloc(MAX);
    memset(batch_command_line, '\0', sizeof(batch_command_line));

    //reads a line from fptr, stores it into batch_command_line
    while(fgets(batch_command_line, MAX, fptr)){
	//remove trailing newline
	batch_command_line[strcspn(batch_command_line, "\n")] = 0;
	printf("Processing batchfile input: %s\n", batch_command_line);

        //parse batch_command_line to set the array COMMANDS[]
        //for example: COMMANDS[0]="ls -a -l", COMMANDS[1]="who", COMMANDS[2]="date"
        int cmd_count = ParseCommands(batch_command_line);

        //execute commands one by one
        for(int i=0; i< cmd_count; i++){
            char *temp = strdup(COMMANDS[i]); //for example: ls -a -l
            temp = strtok(temp, " "); //get the command
            ExecuteCommands(temp, COMMANDS[i]);
            //free temp
			free(temp);
        }
    }
    //free batch_command_line, and close fptr
	free(batch_command_line);
	fclose(fptr);
}

int ParseCommands(char *str){

	int i = 0;

	char *token = strtok(str, ";"); //breaks str into a series of tokens using ;

	while(token != NULL){
		//error checking for possible bad user inputs
		//Removes Spaces at beginning
		while (token[0] == ' ') {
			int size = strlen(token);
			for (int j=0; j<size; j++) {
				token[j] = token[j+1];
			}
		}

		//If after, removing all whitespaces we're left with a NULL char,
		//then the command is empty and will be ignored
		if (token[0] == '\0') {
			token = strtok(NULL, ";");
			continue;
		}

		//Removes all but one whitespace in between args
		for (int j=0; j<strlen(token); j++) {
			//fprintf(stderr,"Token Edit: %s\n", token);
			if (token[j] == ' ' && token[j+1] == ' ') {
				int size = strlen(token);
				for (int k=j; k<size; k++)
					token[k] = token[k+1];
				j--;
			}
		}

        //save the current token into COMMANDS[]
        COMMANDS[i] = token;
        i++;
        //move to the next token
        token = strtok(NULL, ";");
	}

	return i;
}

void ExecuteCommands(char *command, char *full_line)
{
	
	char *args[MAX]; //hold arguments

	MYHISTORY[hist_count%20] = strdup(full_line); //array of commands
	hist_count++;

    //save backup full_line
    char *backup_line = strdup(full_line);
		// execute alias commands
      if(strncmp(command, "alias", strlen("alias")) == 0)
        {
            alias_ptr = execute_alias_command(full_line, alias_ptr);
        }
	// check and run alias command replacement
        else if(! execute_other_command(full_line, alias_ptr))
           {
     
		//parse full_line to get arguments and save them to args[] array
		int arg_count = ParseArgs(full_line, args);

		//restores full_line
        strcpy(full_line, backup_line);
        free(backup_line);

		//check if built-in function is called
		if(strcmp(command, "cd") == 0)
			MyCD(args[0], arg_count);
		else if(strcmp(command, "exit") == 0)
			EXIT_CALLED = 1;
		else if(strcmp(command, "path") == 0)
			MyPath(args, arg_count);
		else if(strcmp(command, "myhistory") == 0)
			MyHistory(args, arg_count);
		else 
			CommandRedirect(args, command, arg_count, full_line);

		//free memory used in ParsedArgs() function
		for(int i=0; i<arg_count-1; i++){
			if(args[i] != NULL){
				free(args[i]);
				args[i] = NULL;
			}
}		
	}
}

int ParseArgs(char *full_line, char *args[]){
	int count = 0;

    //break full_line into a series of tokens by the delimiter space (or " ")
	char *token = strtok(full_line, " ");
	//skip over to the first argument
	token = strtok(NULL, " ");

    while(token != NULL){
        //copy the current argument to args[] array
        args[count] = strdup(token);
        count++;
        //move to the next token (or argument)
        token = strtok(NULL, " ");
    }

    return count + 1;
}

void CommandRedirect(char *args[], char *first_command, int arg_count, char *full_line){
	pid_t pid;
	int status;

	//if full_line contains pipelining and redirection, error displayed
	if (strchr(full_line, '|') != NULL && (strchr(full_line, '<') != NULL || strchr(full_line, '>') != NULL)) {
	    fprintf(stderr,"Command cannot contain both pipelining and redirection\n");
	}
	//if full_line contains "<" or ">", then io_redirect() is called
	else if (strchr(full_line, '<') != NULL || strchr(full_line, '>') != NULL) {
		io_redirect(first_command, full_line);
	}
	//if full_line contains "|", then PipeCommands() is called
	else if (strchr(full_line, '|') != NULL) {
		PipeCommands(args, first_command, arg_count);
	}
	else {//else excute the current command
		//set the new cmd[] array so that cmd[0] hold the actual command
		//cmd[1] - cmd[arg_count] hold the actual arguments
		//cmd[arg_count+1] hold the "NULL"
		char *cmd[arg_count + 1];
		cmd[0] = first_command;
		for (int i=1; i<arg_count; i++)
			cmd[i] = args[i-1];
		cmd[arg_count] = '\0';

		pid = fork();
		if(pid == 0) {
			execvp(*cmd, cmd);
			fprintf(stderr,"%s: command not founds\n", *cmd);
			MyExit();//Ensures child exits after executing command
		}
		else wait(&status);
	}
}

void InteractiveMode(){

	int status = 0;

    //get custom prompt
    prompt = (char*)malloc(MAX);
    printf("Enter custom prompt: ");
    fgets(prompt, MAX, stdin);

    //remove newline from prompt
    if (prompt[strlen(prompt)-1] == '\n') {
        prompt[strlen(prompt)-1] = '\0';
    }

	while(1){
		char *str = (char*)malloc(MAX);

		printf("%s> ", prompt);
		fgets(str, MAX, stdin);

		//error checking for empty commandline
		if (strlen(str) == 1) {
			continue;
		}

		//remove newline from str
		if (str[strlen(str)-1] == '\n') {
			str[strlen(str)-1] = '\0';
		}

		//parse commands
		int cmd_num = ParseCommands(str);//this function can be better designed

		//execute commands that are saved in COMMANDS[] array
		for(int i=0; i < cmd_num; i++){
			char *temp = strdup(COMMANDS[i]);
			temp = strtok(temp, " ");
			ExecuteCommands(temp, COMMANDS[i]);
			//free temp
			free(temp);
		}

		//ctrl-d kill

		free(str);

		// if exit was selected
		if(EXIT_CALLED) {
		    free(prompt);
		    MyExit();
		}
	}
}

void change_directory(char* path) {
    //changing the directory and getting the status
    int return_status = chdir(path);

    //if status is 0 then it will indicate that the proccess was
    //successful else it will say it wasn't
    if (return_status == 0) {
        printf("The directory has been changed successfully!\n");
    }
    else {
        printf("The directory could not be changed!\n");
    }
}
void MyCD(int argc, char* argv[]) {
    //if argc is 1 then we don't have the optional argument therefor
    //it will change to the HOME directory
    if (argc == 1) {
        //this will grab the home directory path from environment variable
        char* home_dir = getenv("HOME");

        //changing the directory
        change_directory(home_dir);
    }
    else {
        //getting the given argument
        char* path = argv[1];

        //changing the directory to the given path
        change_directory(path);
    }
}

void MyExit(){ 
	exit(0);
}

void MyPath(char *args[], int arg_count)
{
	const char **array;
        char *path_var = strdup(ORIG_PATH_VAR ? ORIG_PATH_VAR : ""); // just in case PATH is NULL, very unlikely

        const char *the_dot = ".";
        int j;
        int len=strlen(path_var);
        int nb_colons=0;
        char pathsep = ':';
        int current_colon = 0;

	//if argument just wants to see "path"
	if(arg_count==2)//PATH
	{
		printf("%s\n",ORIG_PATH_VAR);
	}
	//is argument wants to add or remove a "path"
	else if(arg_count==4)//Path +/- "path name"
	{
		// first count how many paths we have
		for (j=0;j<len;j++)
		{
			if (path_var[j]==pathsep)
			{
				nb_colons++;
				path_var[j] = '\0';
			}
		}

		//----------------------------------------------------------------------

		char temp = args[1][0];

		//change to 1 once done
		if(temp=='+')//append path
		{
			printf("Adding pathname: %s\n", args[2]);

			nb_colons++;

			 // allocate the array of strings
                	array=malloc((nb_colons+1) * sizeof(*array));

                	array[0] = path_var;  // first path

                	// rest of paths
                	for (j=0;j<len;j++)
                	{
                	        if (path_var[j]=='\0')
                	        {
                	                current_colon++;
                	                array[current_colon] = path_var+j+1;
                	                if (array[current_colon][0]=='\0')
                	                {
                	                        // special case: add dot if path is empty
                	                        array[current_colon] = the_dot;
                	                }
                	        }
                	}
			current_colon++;
			//adding pathname to "array" of path
			array[nb_colons]=args[2];
			len+=strlen(args[2]);

		}
		else if(temp=='-')//remove path
		{
			//removing...
			printf("Removing pathname: %s\n", args[2]);

                         // allocate the array of strings
                        array=malloc((nb_colons+1) * sizeof(*array));

                        array[0] = path_var;  // first path

                        // rest of paths
                        for (j=0;j<len;j++)
                        {
                                if (path_var[j]=='\0')
                                {
					//skip if path equals the argument (essentially "removing" the wanted argument)
					if(strcmp(args[2],path_var+j+1))
					{
                                        	current_colon++;
                                        	array[current_colon] = path_var+j+1;
                                        	if (array[current_colon][0]=='\0')
                                        	{
                                                	// special case: add dot if path is empty
                                                	array[current_colon] = the_dot;
                                        	}
					}
                                }
                        }
			//checking if path exists
			if(current_colon==nb_colons)
			{
				printf("Pathname not found!\n");
			}
		}

		else//error
		{
			printf("error");
			return;
		}
	}

	else
	{
		return;
	}

	char path2[len];//for some reason contains a random 'n', uses strcpy to empty it.
	char tmp[len];
	strcpy(path2,tmp);//check later if path2 still has random 'n'

	//printing "array" of paths
        for (j=0;j<current_colon+1;j++)
        {
		strcpy(tmp, array[j]);
		strcat(path2,tmp);
		if(!(j==current_colon))
			strcat(path2,":");
        	printf("Path %d: <%s>\n",j,array[j]);
        }

	//save into environmental path
	printf("This was the old Path: %s\n",getenv("PATH"));
	setenv("PATH", path2, 1);
	printf("This is the new Path:%s\n",getenv("PATH"));
	return;
}
// myhistory function
void MyHistory(char *args[], int arg_count){ 
	// command myhistory    
      if (arg_count==1){
		if(hist_count == 0);
		// more than 20 command in history
		if(hist_count >= 20) {
		for(int i=0;i<20;i++){
            printf("%-15d %-15s\n",i+1,MYHISTORY[i]);
}
}
		// less than 20 commands
		if(hist_count < 20){
		for(int i=0;i<hist_count;i++){
            printf("%-15d %-15s\n",i+1,MYHISTORY[i]);
}
}
}
		// clear myhistory command
 		if (arg_count==2){
		if(strcmp(args[0], "-c")==0){
		if(hist_count < 20){
		for(int i=0;i<hist_count;i++){
		MYHISTORY[i]= '\0';
}
}		else{
		for(int i=0;i<20;i++){
                MYHISTORY[i]= '\0';
                }
}
		hist_count=0;
		
}
}
		// print particular command in the myhistory list
		if (arg_count==3){
		if(strcmp(args[0], "-e")==0){
		int x=atoi(args[1]);
		printf("%-15d %-15s\n",x,MYHISTORY[x-1]);
}
}
}
void PipeCommands(char *args[], char *first_command, int arg_count){
                            
        

        
}

void signalHandle(int sig)
{
	pid_t pgid;
	pgid = getpgid(cpid);

	if(sig!=SIGINT && sig!=SIGTSTP)
	{
		printf("\nError: Signal Handler not needed\n\n");
		exit(1);
	}
	else if(sig==SIGINT)
	{
		//printing that SIGINT was recieved
		printf("\n>>> SIGINT recieved\n");
		printf(">>> [doing necessary action ...]\n");

		//Handling Sending Signals
		if(ppid==pgid)
		{
			printf("<<<Sending Signal to SubProcesses>>>\n\n");
			kill(pgid,sig);
		}
	}
	else
	{
		// printing that SIGTSTP was received
		printf("\n>>> SIGTSTP received\n");
		printf(">>> [doing necessary action ...]\n");
		// TO DO: enter code for handling SIGTSTP signal
		if(ppid==pgid)
		{
			printf("<<<Terminating group>>>\n\n");
			kill(pgid,sig);
		}
	}
	// resetting
	signal(sig, signalHandle);
	printf("Back to \"Parent Function\">>>\n\n");
	return;  							
}
 	

//This function redirects output to and from a file
void io_redirect(char *command, char* full_line) {
	const char s[2] = " ";
	char *token;
	
	//variables to store input line and tokenize the arguments
	char* arg1;
	char* arg2;
	char* mode;

	//file pointer to point to the file for IO operations
	FILE* fp;
	
	//tokenizes the command line input
	token = strtok(full_line, s);
	arg1 = token;

	//loops through the rest of the arguments given
	while( token != NULL ){
		printf(" %s\n", token);
		token = strtok(NULL, s);

		mode = token;
		printf(" %s\n", mode);
	
		//output redirection
		if(mode && *mode == '>'){
			 token = strtok(NULL, s);
			 fp = fopen(token, "w");
			 if(fp == NULL){
			 	printf("Error opening file\n");
				exit(1);
			 }

		}

		//input redirection
		else if(mode && *mode == '<'){
			printf("Mode: <\n");
		}
	}



}


