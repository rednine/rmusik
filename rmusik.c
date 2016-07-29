//SG

//Random Music Player - rmPlayer

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

/* Datatypes */

#define RM_MAX_PATH_LEN	  1024
#define MAX_DIR_ENTRIES   512
#define MAX_DIR_ENTRY_LEN 256
#define MAX_ACTIONS       5

typedef enum
{
	ACT_GO_DOWN    = 1,
	ACT_GO_UP      = (1 << 1),
	ACT_FILE_PLAY  = (1 << 2),
	ACT_SELECT_DIR = (1 << 3),
	ACT_MAX		   = (1 << 4)
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
	int numDirs;
	int numFiles;
	struct
	{
		dirEntry_t entryType;
	    char entryName[MAX_DIR_ENTRY_LEN];
	}dirEntries[MAX_DIR_ENTRIES];
	playBackState_t playbackState;
	pid_t playerPid;
}playerInstance_t;

/* Global variables */
#define IS_VALID_ACTION(a) (((a) == ACT_GO_DOWN) || ((a) == ACT_GO_UP) || ((a) == ACT_FILE_PLAY))
playerInstance_t rmPlayer;
extern int errno;

/* Functions */
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
		actionMask = ACT_GO_DOWN;
	else
		actionMask = ACT_GO_UP | ACT_GO_DOWN | ACT_FILE_PLAY;

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
		action = randAction % MAX_ACTIONS;
		//printf("\n %s : action = %d randAction = %X actionMask & action = %d\n", __FUNCTION__, action, randAction, (actionMask & action)); 

		if(IS_VALID_ACTION(action))
		{
			//some valid action was generated
			//now check with mask
			if((actionMask & action) == action)
			{
				retVal = action;
				break;
			}
		}
	}

	return retVal;
}

void CheckErrNo()
{
	/* check and print errno*/
	printf("\n %s : error = %d %s \n", __FUNCTION__, errno, strerror(errno));
}

int SelectRandomDirEntry(dirEntry_t entry)
{
	int dirIndex = 0;
	int numEntries = rmPlayer.numDirs + rmPlayer.numFiles;

	while(1)
	{
		dirIndex = rand() % (numEntries + 1);
		
		//printf("\n %s : numEntries = %d dirIndex = %d \n", __FUNCTION__, numEntries, dirIndex);

		if((dirIndex >=0 && dirIndex <=numEntries) &&
				(entry == rmPlayer.dirEntries[dirIndex].entryType) &&
				(strcmp(rmPlayer.dirEntries[dirIndex].entryName, ".") != 0) &&
				(strcmp(rmPlayer.dirEntries[dirIndex].entryName, "..") != 0))
			break;
	}

	return dirIndex;
}

void StartPlayBack()
{	
	char *argv[3];
	char cmdBuf[2048];
	
	printf("\n %s : Starting to playback %s [PID=%d]\n", __FUNCTION__, rmPlayer.nextSong, getpid());
	
	memset(cmdBuf, 0x00, (sizeof(char) * 2048));
	
	sprintf(cmdBuf, "-q %s", rmPlayer.nextSong);
	
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

			while(!errno && (dirEntry = readdir(dirHandle))!= NULL)
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
						rmPlayer.numFiles++;
						//printf("\n %s : Found file numFiles = %d\n", __FUNCTION__, rmPlayer.numFiles);
					}
					else
						printf("\n %s : Found OTHER TYPE = %d %d\n", __FUNCTION__, /*(sb.st_mode & S_IFMT == S_IFDIR)*/S_ISDIR(sb.st_mode), /*(sb.st_mode & S_IFMT == S_IFREG)*/S_ISREG(sb.st_mode));

					index++;
				}
			}//end while

			if(errno)
			{
				printf("\n %s : %d : Error reading dir entries of %s \n", __FUNCTION__, __LINE__, rmPlayer.curDir);
				return -1;
			}
			else
			{
				//Update the state varibles w.r.t curDir
				rmPlayer.isRoot = (!strcmp(rmPlayer.curDir, "/"));
				rmPlayer.hasNoChildDir = ((rmPlayer.numDirs <= 0) && (rmPlayer.numFiles > 0));
				rmPlayer.containsNoFiles = ((rmPlayer.numDirs > 0) && (rmPlayer.numFiles <= 0));
				rmPlayer.isEmpty = ((rmPlayer.numDirs <= 0) && (rmPlayer.numFiles <= 0));
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
		
		//printf("\n %s : strPrt = %s\n", __FUNCTION__, strPtr);	
		if((len == 1)  && (strPtr[0] == '/'))
		{
			//We have already at root, then dont strip the '/'
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

int SelectNextSongDir()
{
	int retVal = 0;
	playerAction_t nextAction = 0;

	while(1)
	{
		//Get all possible actions at this level
		int actionMask = GetNextActionMask();
		printf("\n %s : Get Next Action Mask = %d \n", __FUNCTION__, actionMask);
		nextAction = ChooseNextAction(actionMask);

		if(nextAction == ACT_GO_DOWN)
		{
			//printf("\n %s : Selecting Child Dir\n", __FUNCTION__);
			SelectChildDir();
			UpdatePlayerInstance();
		}
		else if(nextAction == ACT_GO_UP)
		{
			//printf("\n %s : Selecting Parent Dir\n", __FUNCTION__);
			SelectParentDir();
			UpdatePlayerInstance();
		}
		else if(nextAction == ACT_FILE_PLAY)
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
	
	return retVal;
}

static void signal_handler (int signo)
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
		//rmPlayer.playbackState = PLAYBACK_STOPPED;
	}	
	else
		printf("\n %s : Unknown or unhandled signal %d\n", __FUNCTION__, signo);
}

void main(int argc, char *argv[])
{
	int option = 0;
	int retVal = 0;

	/* /media/sunil/02C6FF22C6FF151F/Songs/mixed */
	
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
		//Use the user supplied start dir
		memcpy(rmPlayer.curDir, argv[1], strlen(argv[1]));

	srand(time(NULL));
    
    UpdatePlayerInstance();
    
    rmPlayer.playbackState = PLAYBACK_INVALID;
    rmPlayer.playerPid = 0;

	do
	{
		printf("\n1. Choose next song \n2. Play song \n3. Exit\nEnter your option: Parent process = %d", getpid());
		scanf("%d", &option);
		switch(option)
		{
				case 1:
				{
					SelectNextSongDir();
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
					break;
				}
				default:
				{
					printf("\n Unknown option...\n");
					break;
				}
		}
	}while(option != 3);

	exit(retVal);
}
