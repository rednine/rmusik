//SG

//Random Music Player - rmusik

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <fcntl.h>

/* Datatypes */
typedef enum _bool {false, true} bool;

/* https://www.experts-exchange.com/questions/23250824/TCP-server-in-C-How-to-avoid-Interrupted-system-call.html */

#define RM_MAX_PATH_LEN	  1024
#define MAX_DIR_ENTRIES   512
#define MAX_DIR_ENTRY_LEN 256
#define MAX_ACTIONS       5
#define RM_MSG_Q_KEY      (('r'<<24) | ('m'<<16) | ('s'<<8) | ('e'))
#define MAX_RETRIES       10

#define SUPPORTED_AUDIO_FILE_FORMAT_EXTENSION_STR "mp3"
#define FILE_EXTENSION_DELIM_STR                  "."
 
union semun {
    int val;
    struct semid_ds *buf;
    ushort *array;
};

typedef enum
{
	ACT_GO_DOWN    = 1,
	ACT_GO_UP      = (1 << 1),	//2
	ACT_FILE_PLAY  = (1 << 2),	//4
	ACT_SELECT_DIR = (1 << 3),	//8
	ACT_MAX		   = (1 << 4)	//16
}playerAction_t;

typedef enum
{
	DIR_ENTRY_TYPE_DIR,
	DIR_ENTRY_TYPE_FILE,
	DIR_ENTRY_TYPE_OTHER
}dirEntry_t;

typedef enum
{
	PLAYBACK_PLAYING = 0x1000,
	PLAYBACK_STOPPED = 0x2000,
	PLAYBACK_INVALID = 0xFFFF
}playBackState_t;

typedef struct
{
	short int isRoot;
	short int isEmpty;
	short int hasNoChildDir;
	short int containsNoFiles;
	char nextSong[RM_MAX_PATH_LEN];
	char prevSong[RM_MAX_PATH_LEN];
	char curSong[RM_MAX_PATH_LEN];
	char curDir[RM_MAX_PATH_LEN];
	char startingDir[RM_MAX_PATH_LEN];
	int numDirs;
	int numFiles;
	struct
	{
		dirEntry_t entryType;
	    char entryName[MAX_DIR_ENTRY_LEN];
	}dirEntries[MAX_DIR_ENTRIES];
	playBackState_t playbackState;
	pid_t playerPid;
	int semId;
	int devRandFd;
	playerAction_t nextAction;
	playerAction_t prevAction;
}playerInstance_t;

/* Global variables */
#define IS_VALID_ACTION(a) (((a) == ACT_GO_DOWN) || ((a) == ACT_GO_UP) || ((a) == ACT_FILE_PLAY))

playerInstance_t rmPlayer;

extern int errno;

#define PRINT_ENUM(e) #e

/************ Functions ***************/

char *printActionStr(playerAction_t a)
{
	switch(a)
	{
		case ACT_GO_DOWN: 		return "ACT_GO_DOWN";
		case ACT_GO_UP: 		return "ACT_GO_UP";
		case ACT_FILE_PLAY: 	return "ACT_FILE_PLAY";
		case ACT_SELECT_DIR: 	return "ACT_SELECT_DIR";
		
		default:
		case ACT_MAX: 
			return "Unknown action";
	}
}

int GetRandomNumber()
{	
	char data[1];
	int result = read(rmPlayer.devRandFd, (void *)data, 1);
	if(result < 0)
	{
		printf("\n GetRandomNumber : failed to read /dev/urandom, falling back to rand()\n");
		return rand();
	}
	
	return((int)data[0]);	
}

int GetNextActionMask()
{
	int actionMask = 0;


	if(rmPlayer.isRoot > 0)
		actionMask = ACT_GO_DOWN;
	else if (rmPlayer.isEmpty > 0)
		actionMask = ACT_GO_UP;
	else if(rmPlayer.hasNoChildDir > 0 && rmPlayer.containsNoFiles <= 0)
		actionMask = ACT_GO_UP | ACT_FILE_PLAY;
	else if (rmPlayer.hasNoChildDir <=0 && rmPlayer.containsNoFiles > 0)
		actionMask = ACT_GO_UP | ACT_GO_DOWN;
	else if (rmPlayer.hasNoChildDir <=0 && rmPlayer.containsNoFiles <= 0)
		actionMask = ACT_GO_UP | ACT_GO_DOWN | ACT_FILE_PLAY;
	else
	{
		actionMask = ACT_GO_UP | ACT_GO_DOWN | ACT_FILE_PLAY;
		printf("\n Add all - Default case");
	}

	//printf("\n isRoot %d hasNoChildDir %d containsNoFiles %d actionMask %d", rmPlayer.isRoot, rmPlayer.hasNoChildDir, rmPlayer.containsNoFiles, actionMask);
	return actionMask;
}

int ChooseNextAction(int actionMask)
{
	int randAction = 0;
	int action = 0;
	int retVal = -1;

	//srand(time(NULL));

	while(1)
	{
		randAction = rand();
		//randAction = GetRandomNumber();
		action = randAction % ACT_MAX;		

		if(IS_VALID_ACTION(action))
		{
			//some valid action was generated
			//now check with mask
			if((actionMask & action) == action)
			{
				printf("\n %s : action = %d randAction = 0x%X actionMask & action = %s\n", __FUNCTION__, action, randAction, printActionStr(actionMask & action)); 
				retVal = action;
				break;
			}
		}
	}

	return retVal;
}

bool IsMp3File(char *file)
{
	bool found = false;
	char *saveptr = NULL, *token = NULL, *tmpStr = NULL, *tmpStr1 = NULL;
	
	tmpStr = (char *)malloc(sizeof(char) * 2048);
	
	if(!tmpStr)
	{
		printf("\n %s : %d Malloc failed, exiting...\n", __FUNCTION__, __LINE__);
		exit(1);
	}
	
	tmpStr1 = tmpStr;
	
	strcpy(tmpStr, file);
	
	for(tmpStr; token = strtok_r(tmpStr, FILE_EXTENSION_DELIM_STR, &saveptr); tmpStr=NULL)
	{									
		if(token)
		{
			if(!strcmp(token, SUPPORTED_AUDIO_FILE_FORMAT_EXTENSION_STR))
			{
				//printf("\n %s is a MP3 file\n", file);
				found = true;
				break;
			}
		}
	}
	
	if(tmpStr1)
		free(tmpStr1);	
		
	return found;			
}

void CheckErrNo()
{
	/* check and print errno*/
	printf("\n %s : error = %d %s \n", __FUNCTION__, errno, strerror(errno));
}

int CheckIsAtStartingDir(char *curDir, char *startingDir)
{
	char aCurDir[RM_MAX_PATH_LEN];
	char aStartingDir[RM_MAX_PATH_LEN];

	memcpy(aCurDir, curDir, RM_MAX_PATH_LEN);
	memcpy(aStartingDir, startingDir, RM_MAX_PATH_LEN);

	//strip off the tailing '/' in the starting dir if present
	if(aStartingDir[strlen(aStartingDir) - 1] == '/')
		aStartingDir[strlen(aStartingDir) - 1] = '\0';

	if(aCurDir[strlen(aCurDir) - 1] == '/')
		aCurDir[strlen(aCurDir) - 1] = '\0';

	return (strcmp(aCurDir, aStartingDir) == 0);
}

/*
** initsem() -- more-than-inspired by W. Richard Stevens' UNIX Network
** Programming 2nd edition, volume 2, lockvsem.c, page 295.
*/
int rm_sem_init()
{
	int i;
    union semun arg;
    struct semid_ds buf;
    struct sembuf sb;
    int semid;
    int nsems = 1;

    semid = semget(RM_MSG_Q_KEY, nsems, IPC_CREAT | IPC_EXCL | 0666);

#if 0
    if (semid >= 0) 
    { 
		/* we got it first */
        sb.sem_op = 1; 
        sb.sem_flg = 0;
        arg.val = 1;

        for(sb.sem_num = 0; sb.sem_num < nsems; sb.sem_num++) 
        { 
            /* do a semop() to "free" the semaphores. */
            /* this sets the sem_otime field, as needed below. */
            if (semop(semid, &sb, 1) == -1) 
            {
                int e = errno;
                semctl(semid, 0, IPC_RMID); /* clean up */
                errno = e;
                return -1; /* error, check errno */
            }
        }
    }
    else if (errno == EEXIST) 
    {
		/* someone else got it first */
        int ready = 0;

        semid = semget(RM_MSG_Q_KEY, nsems, 0); /* get the id */
        if (semid < 0) return semid; /* error, check errno */

        /* wait for other process to initialize the semaphore: */
        arg.buf = &buf;
        for(i = 0; i < MAX_RETRIES && !ready; i++) {
            semctl(semid, nsems-1, IPC_STAT, arg);
            if (arg.buf->sem_otime != 0) {
                ready = 1;
            } else {
                sleep(1);
            }
        }
        if (!ready) {
            errno = ETIME;
            return -1;
        }
    } 
    else 
    {
        return semid; /* error, check errno */
    }
#endif

    return semid;
}

void rm_sem_dispval(int semid, int member)
{
        int semval;

        semval = semctl(semid, member, GETVAL, 0);
        printf("\n %s : semval for member %d is %d\n", __FUNCTION__, member, semval);
}

void rm_sem_wait()
{
	struct sembuf sb;
	
	rm_sem_dispval(rmPlayer.semId, 0);
    
    sb.sem_num = 0;
    sb.sem_op = -1;  /* set to allocate resource */
    sb.sem_flg = SEM_UNDO;
    
    if (semop(rmPlayer.semId, &sb, 1) == -1) 
    {
        if(errno == EINTR)
        {
			//retry once more...
			if (semop(rmPlayer.semId, &sb, 1) == -1)
				CheckErrNo();
			else
				printf("\n %s : sem_wait OK...\n", __FUNCTION__);
		}
		else
			CheckErrNo();
    }
} 

void rm_sem_post()
{
	struct sembuf sb;
    
    sb.sem_num = 0;
    sb.sem_op = 1;  /* free resource */
    sb.sem_flg = SEM_UNDO;
    
    if (semop(rmPlayer.semId, &sb, 1) == -1) 
    {
        CheckErrNo();
    }
    else
		printf("\n %s : sem_post OK...\n", __FUNCTION__);
		
	rm_sem_dispval(rmPlayer.semId, 0);	
}

void rm_sem_destroy()
{
	union semun arg;
	
	if (semctl(rmPlayer.semId, 0, IPC_RMID, arg) == -1) 
	{
        CheckErrNo();
    }
    else
		printf("\n %s : sem_destroy OK...\n", __FUNCTION__);
}

int SelectRandomDirEntry(dirEntry_t entry)
{
	int dirIndex = 0;
	int numEntries = rmPlayer.numDirs + rmPlayer.numFiles;

	while(1)
	{
		dirIndex = rand() % (numEntries);
		//dirIndex = GetRandomNumber() % (numEntries);
		
		//printf("\n %s : numEntries = %d dirIndex = %d \n", __FUNCTION__, numEntries, dirIndex);

		if((dirIndex >=0 && dirIndex <numEntries) && (entry == rmPlayer.dirEntries[dirIndex].entryType))
		{
			if(rmPlayer.dirEntries[dirIndex].entryType == DIR_ENTRY_TYPE_DIR)
			{
				if((strcmp(rmPlayer.dirEntries[dirIndex].entryName, ".") != 0) &&
							(strcmp(rmPlayer.dirEntries[dirIndex].entryName, "..") != 0))
				break;
			}
			else //if(rmPlayer.dirEntries[dirIndex].entryType == DIR_ENTRY_TYPE_FILE)
			{
				//TODO: Make sure the same file is not chosen again...
				
				printf("\n Selected file %s , checking...\n", rmPlayer.dirEntries[dirIndex].entryName);
				
				if(IsMp3File(rmPlayer.dirEntries[dirIndex].entryName) == true)
					break;	//Some mp3 file has been found
				else
					continue; //Some non mp3 file has been found, continue search
			}
		}
	}

	return dirIndex;
}

void StartPlayBack()
{	
	char *argv[3];
	//char cmdBuf[2048];
	
	printf("\n %s : Starting to playback %s [PID=%d]\n", __FUNCTION__, rmPlayer.nextSong, getpid());
	
	//memset(cmdBuf, 0x00, (sizeof(char) * 2048));
	
	//sprintf(cmdBuf, "-q %s", rmPlayer.nextSong);
	
	argv[0] = "mpg321";
	argv[1] = rmPlayer.nextSong;
	argv[2] = NULL;
	if(execv("/usr/bin/mpg321", argv) < 0)
		CheckErrNo();
}

void PlaySong()
{
	if(rmPlayer.playbackState == PLAYBACK_PLAYING)
	{
		//There is a playback going on, kill that child process first
		if(kill(rmPlayer.playerPid, SIGKILL)!=0)
		{
			CheckErrNo();
			printf("\n %s : Unable to kill previous player process, exiting...\n", __FUNCTION__);
			exit(-1);
		}
		else
		{
			//wait for previous song to have ended
			rm_sem_wait();
			printf("\n %s : Killed child player process..\n", __FUNCTION__);
			rmPlayer.playbackState = PLAYBACK_STOPPED;
		}
	}

	rmPlayer.playerPid = fork();
		
	if(rmPlayer.playerPid < 0)
	{
		CheckErrNo();
	}
	else if( rmPlayer.playerPid == 0)
	{
		//Child process
		StartPlayBack();
	}
	else
	{
		//Parent process
		printf("\n %s : playback process created [PID=%d] [CH PID = %d]\n", __FUNCTION__, getpid(), rmPlayer.playerPid);
		rmPlayer.playbackState = PLAYBACK_PLAYING;
	}
}

int UpdatePlayerInstance()
{
	int retVal = 0;
	struct stat sb;
	
	printf("\n %s : Scanning %s ...\n", __FUNCTION__, rmPlayer.curDir);
	
	rmPlayer.isRoot = -1;
	rmPlayer.hasNoChildDir = -1;
	rmPlayer.containsNoFiles = -1;
	rmPlayer.isEmpty = -1;
	rmPlayer.numFiles = 0;
	rmPlayer.numDirs = 0;

	if(stat(rmPlayer.curDir, &sb) < 0)
	{
		CheckErrNo();
		return -1;
	}
	else
	{		
		//Valid dir, proceed...
		memset(rmPlayer.dirEntries, 0x0, sizeof(rmPlayer.dirEntries));

		DIR *dirHandle = opendir(rmPlayer.curDir);
		int index = 0;

		if(dirHandle)
		{
			struct dirent *dirEntry;

			while(/*!errno &&*/ (dirEntry = readdir(dirHandle))!= NULL)
			{
				char name[RM_MAX_PATH_LEN];

				memset(name, 0x0, sizeof(name));

				memcpy(name, rmPlayer.curDir, sizeof(rmPlayer.curDir));
				if(name[strlen(name)-1] != '/')
					strcat(name, "/");
				strcat(name, dirEntry->d_name);
				
				//printf("\n %s : Got entry %s\n", __FUNCTION__, name);
				
				if(stat(name, &sb) < 0)
				{
					CheckErrNo();
					return -1;
				}
				else
				{
					rmPlayer.dirEntries[index].entryType = (S_ISDIR(sb.st_mode)/*sb.st_mode & S_IFMT == S_IFDIR*/) ? DIR_ENTRY_TYPE_DIR :
															(S_ISREG(sb.st_mode)/*(sb.st_mode & S_IFMT == S_IFREG)*/ ? DIR_ENTRY_TYPE_FILE : DIR_ENTRY_TYPE_OTHER);
					memcpy(rmPlayer.dirEntries[index].entryName, dirEntry->d_name, strlen(dirEntry->d_name));

					if(rmPlayer.dirEntries[index].entryType == DIR_ENTRY_TYPE_DIR)
					{
						rmPlayer.numDirs++;
						//printf("\n %s : Found dir numDirs = %d\n", __FUNCTION__, rmPlayer.numDirs);
					}
					else if (rmPlayer.dirEntries[index].entryType == DIR_ENTRY_TYPE_FILE)
					{
						if(IsMp3File(rmPlayer.dirEntries[index].entryName) == true)
							rmPlayer.numFiles++;
						//printf("\n %s : Found file numFiles = %d\n", __FUNCTION__, rmPlayer.numFiles);
					}
					else
						printf("\n %s : Found OTHER TYPE = %d %d\n", __FUNCTION__, /*(sb.st_mode & S_IFMT == S_IFDIR)*/S_ISDIR(sb.st_mode), /*(sb.st_mode & S_IFMT == S_IFREG)*/S_ISREG(sb.st_mode));

					index++;
				}
			}//end while

			if(errno && errno != EINTR)
			{
				printf("\n %s : %d : Error reading dir entries of %s \n", __FUNCTION__, __LINE__, rmPlayer.curDir);
				CheckErrNo();
				return -1;
			}
			else
			{
				//Update the state varibles w.r.t curDir
				rmPlayer.isRoot = CheckIsAtStartingDir(rmPlayer.curDir, rmPlayer.startingDir);
				rmPlayer.hasNoChildDir = ((rmPlayer.numDirs <= 2) && (rmPlayer.numFiles > 0));
				rmPlayer.containsNoFiles = ((rmPlayer.numDirs > 2) && (rmPlayer.numFiles <= 0));
				rmPlayer.isEmpty = ((rmPlayer.numDirs <= 2) && (rmPlayer.numFiles <= 0));
				//printf("\n @ isRoot check curDir = %s startingDir = %s isRoot %d", rmPlayer.curDir, rmPlayer.startingDir, rmPlayer.isRoot);
			}
		}
		else
		{
			printf("\n %s : %d : Error in opening dir %s\n", __FUNCTION__, __LINE__, rmPlayer.curDir);
			return -1;
		}
	}

	return 0;
}

int SelectParentDir()
{
	int retVal = 0;
	struct stat sb;

	if(stat(rmPlayer.curDir, &sb) < 0)
	{
		CheckErrNo();
		return -1;
	}
	else
	{			
		//Remove the last dir name in the str to traverse one level above
		int len = strlen(rmPlayer.curDir);
		char *strPtr = rmPlayer.curDir + (len-1);
		
		if((len == 1)  && (strPtr[0] == '/'))
		{
			//We are already at root, then dont strip the '/'
			return 0;
		}
		else
		{
			//Strip if the path ends with a '/'
			if(*strPtr == '/')
				*strPtr = '\0';

			//Now traverse backwards until we find a '/'
			while(*strPtr != '/')
				strPtr--;
				
			*strPtr = '\0';
			
			printf("\n %s : Selecting Parent Dir %s\n", __FUNCTION__, rmPlayer.curDir);
		}
	}
	
	return 0;
}

int SelectChildDir()
{
	struct stat sb;

	if(stat(rmPlayer.curDir, &sb) < 0)
	{
		CheckErrNo();
		return -1;
	}
	else
	{
		int index = SelectRandomDirEntry(DIR_ENTRY_TYPE_DIR);
		if(rmPlayer.curDir[strlen(rmPlayer.curDir)-1] != '/')
			strcat(rmPlayer.curDir , "/");
		strcat(rmPlayer.curDir , rmPlayer.dirEntries[index].entryName);
		printf("\n %s : Selecting Child Dir %s\n", __FUNCTION__, rmPlayer.dirEntries[index].entryName);
	}

	return 0;
}

int SelectNextSong()
{
	int retVal = 0;
	bool isDirChanged = false;
	int actionMask = 0;

	rmPlayer.nextAction = 0;
	rmPlayer.prevAction = 0;
	
	//We force to start from a different directory, to avoid playing from the same directory...
	
	while(1)
	{
		//Get all possible actions at this level		
		actionMask = GetNextActionMask();
		
		// if only one action is possible
		if((actionMask == ACT_GO_DOWN) || (actionMask == ACT_GO_UP))
		{
			// just do what actionMask says	
		}
		else
		{
			// Adjust actionMask only when multiple actions are possible
			if(rmPlayer.prevAction == ACT_GO_DOWN)
				actionMask &= ~(ACT_GO_UP);
			else if(rmPlayer.prevAction == ACT_GO_UP)
				actionMask &= ~(ACT_GO_DOWN);
			else
			{
				//printf("\n%s : not adjusting actionMask", __FUNCTION__);
				//First Run of this function
			}
		}
		
		//printf("\n %s : Adjusted actionMask %d", __FUNCTION__, actionMask);
		
		rmPlayer.nextAction = ChooseNextAction(actionMask);
		
		if(rmPlayer.nextAction == ACT_GO_DOWN)
		{
			printf("\n %s : Selecting Child Dir\n", __FUNCTION__);
			SelectChildDir();
			
			if (UpdatePlayerInstance() < 0)
			{
				isDirChanged = false;
			}
			else 
			{
				if (rmPlayer.isEmpty <= 0)
				{
					isDirChanged = true;			
					rmPlayer.prevAction = rmPlayer.nextAction;
				}
				else
				{
					//empty dir
					printf("\n %s: Empty dir %s, will try again...", __FUNCTION__, rmPlayer.curDir);
				}
			}
			
			continue;
		}
		else if(rmPlayer.nextAction == ACT_GO_UP)
		{
			printf("\n %s : Selecting Parent Dir\n", __FUNCTION__);
			SelectParentDir();

			if (UpdatePlayerInstance() < 0)
			{
				isDirChanged = false;
			}
			else
			{
				rmPlayer.prevAction = rmPlayer.nextAction;
				isDirChanged = true;

				if(rmPlayer.isRoot)
				{
					printf("\n Have reached the starting dir...");
				}
				
				continue;
			}
		}
		else if((rmPlayer.nextAction == ACT_FILE_PLAY) /* && (isDirChanged == true) */)
		{
			printf("\n %s : Selecting Play file\n", __FUNCTION__);
			
			//Its now ACT_FILE_PLAY, that means a next dir has been selected, select a song now
			int index = SelectRandomDirEntry(DIR_ENTRY_TYPE_FILE);
			memset(rmPlayer.prevSong, 0x0, sizeof(rmPlayer.prevSong));
			memcpy(rmPlayer.prevSong, rmPlayer.nextSong, strlen(rmPlayer.nextSong));
			memset(rmPlayer.nextSong, 0x0, sizeof(rmPlayer.nextSong));
			memcpy(rmPlayer.nextSong, rmPlayer.curDir,strlen(rmPlayer.curDir));
			if(rmPlayer.nextSong[strlen(rmPlayer.nextSong)-1] != '/')
				strcat(rmPlayer.nextSong, "/");
			strcat(rmPlayer.nextSong, rmPlayer.dirEntries[index].entryName);
			break;
		}
		else
		{
			//unhandled case
			printf("\n %s : Action = %s isDirChanged = %s\n", __FUNCTION__, printActionStr(rmPlayer.nextAction), (isDirChanged == true)? "true" : "false");
		}
	}

	return retVal;
}

int ExitPlayer()
{
	int retVal = 0;
	
	if(rmPlayer.playbackState == PLAYBACK_PLAYING)
	{
		//There is a playback going on, kill that child process first
		if(kill(rmPlayer.playerPid, SIGKILL) != 0)
		{
			CheckErrNo();
			retVal = -1;
		}
	}
	
	rm_sem_destroy();
	
	close(rmPlayer.devRandFd);
	
	return retVal;
}

void sigaction_handler(int signo, siginfo_t * siginfo, void *data)
{
	if((signo == SIGINT) || (signo == SIGTERM))
	{
		printf("\n %s : SIGINT/SIGTERM received  - killing child player process...\n", __FUNCTION__);
		exit(ExitPlayer());
	}
	else if(signo == SIGCHLD)
	{
		int childStatus = 0;
		pid_t childPid= 0;
		
		printf("\n %s : SIGCHLD received - waiting child player process to exit[%d]...", __FUNCTION__, rmPlayer.playerPid);
		childPid = waitpid(-1/*rmPlayer.playerPid*/, &childStatus, WNOHANG);
		if(childPid < 0)
			CheckErrNo();
		else
			printf("...DONE [CHILD PID=%d]\n", childPid);
	
		rmPlayer.playbackState = PLAYBACK_STOPPED;
		
		//Dont release the semphore if child exited normally
		if(siginfo->si_code != CLD_EXITED)
		{
			//Indicate completion of killing of previous child
			rm_sem_post(); 
			printf("\n Child exited abnormally, releasing semaphore \n"); 
		}
	}	
	else
		printf("\n %s : Unknown or unhandled signal %d\n", __FUNCTION__, signo);
		
	rm_sem_dispval(rmPlayer.semId, 0);
}

static void signal_handler (int signo)
{
	#if 0
	if((signo == SIGINT) || (signo == SIGTERM))
	{
		printf("\n %s : SIGINT/SIGTERM received  - killing child player process...\n", __FUNCTION__);
		exit(ExitPlayer());
	}
	else if(signo == SIGCHLD)
	{
		int childStatus = 0;
		pid_t childPid= 0;
		
		printf("\n %s : SIGCHLD received - waiting child player process to exit[%d]...", __FUNCTION__, rmPlayer.playerPid);
		childPid = waitpid(-1/*rmPlayer.playerPid*/, &childStatus, WNOHANG);
		if(childPid < 0)
			CheckErrNo();
		else
			printf("...DONE [CHILD PID=%d]\n", childPid);
	
		rmPlayer.playbackState = PLAYBACK_STOPPED;
		
		//Indicate completion of killing of previous child
		rm_sem_post(); 
	}	
	else
		printf("\n %s : Unknown or unhandled signal %d\n", __FUNCTION__, signo);
	#endif
}

void registerSignalHandlers()
{
#if 0
	if (signal (SIGINT, signal_handler) == SIG_ERR) {
		fprintf (stderr, "Cannot handle SIGINT!\n");
		exit (EXIT_FAILURE);
	}
	
	if (signal (SIGTERM, signal_handler) == SIG_ERR) {
		fprintf (stderr, "Cannot handle SIGTERM!\n");
		exit (EXIT_FAILURE);
	}	
	
	if (signal (SIGCHLD, signal_handler) == SIG_ERR) {
		fprintf (stderr, "Cannot handle SIGCHLD!\n");
		exit (EXIT_FAILURE);
	}
#else

	struct sigaction sigAct = 
	{
		.sa_handler = NULL,
		.sa_sigaction = sigaction_handler,
		.sa_flags = (/*SA_NOCLDSTOP |*/ SA_RESTART),
		.sa_restorer = NULL
	};
	
#if 0
	struct sigaction sigAct; 
	
	sigAct.sa_handler = &signal_handler;
	sigAct.sa_sigaction = NULL;
	sigAct.sa_flags = (SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART);
	sigAct.sa_restorer = NULL;
#endif
	
	sigfillset(&sigAct.sa_mask);
	
	if(sigaction(SIGINT, &sigAct, NULL) < 0) {
		fprintf (stderr, "Cannot handle SIGINT!\n");
		exit (EXIT_FAILURE);
	}

	if(sigaction(SIGTERM, &sigAct, NULL) < 0) {
		fprintf (stderr, "Cannot handle SIGTERM!\n");
		exit (EXIT_FAILURE);
	}
	
	if(sigaction(SIGCHLD, &sigAct, NULL) < 0) {
		fprintf (stderr, "Cannot handle SIGCHLD!\n");
		exit (EXIT_FAILURE);
	}
#endif
}

int main(int argc, char *argv[])
{
	int option = 0;
	int retVal = 0;
	union semun arg;
	
	registerSignalHandlers();
	
	if( argc < 2)
	{
		printf("\n %s : No start dir specified, using current working directory...\n", argv[0]);
		if(!getcwd(rmPlayer.curDir, RM_MAX_PATH_LEN))
		{
			printf("\n %s : error in getting current working dir\n", argv[0]);
			exit(1);
		}
	}
	else
	{
		//Use the user supplied start dir
		memcpy(rmPlayer.curDir, argv[1], strlen(argv[1]));
		memcpy(rmPlayer.startingDir, argv[1], strlen(argv[1]));
	}
	
	rmPlayer.devRandFd = open("/dev/urandom", O_RDONLY|O_NONBLOCK);
	
	if(rmPlayer.devRandFd < 0)
	{
		printf("\nFailed to open /dev/urandom, exiting... \n");
		exit(1);
	}
	
	srand(GetRandomNumber());
    
    UpdatePlayerInstance();
    
    rmPlayer.playbackState = PLAYBACK_INVALID;
    rmPlayer.playerPid = 0;
    
    rmPlayer.semId = rm_sem_init();
    
    /* initialize semaphore #0 to 1: */ 
    arg.val = 0; 
    if (semctl(rmPlayer.semId, 0, SETVAL, arg) == -1) { 
		perror("semctl"); 
        exit(1); 
    } 
    
    rm_sem_dispval(rmPlayer.semId, 0);

	do
	{
		printf("\n1. Choose next song \n2. Play song \n3. Exit\nEnter your option: ");
		scanf("%d", &option);
		switch(option)
		{
				case 1:
				{
					SelectNextSong();
					printf("\n Next Song %s, previous song %s\n", rmPlayer.nextSong, rmPlayer.prevSong);
					break;
				}
				case 2:
				{
					printf("\n Playing %s song...\n", rmPlayer.nextSong);
					PlaySong();
					break;
				}
				case 3:
				{
					printf("\n Exiting...\n");
					retVal = ExitPlayer();
					exit(0);
				}
				default:
				{
					printf("\n Unknown option...\n");
					break;
				}
		}
	}while(true);

	exit(retVal);
}
