/*
-parseCmd
-openMsg
-compressMsg
-encryptMsg
-openAudio
-stego
-output
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include <unistd.h>    /* for fork */
#include <sys/types.h> /* for pid_t */
#include <sys/wait.h>  /* for wait */

#include "utils/wavtool.h"
#include "compress/compress.h"
#include "crypto/crypto.h"
#include "stego/stego.h"
#include "stego_main.h"

typedef int bool;
#define true 1
#define false 0

bool isEncode = true;
char *msgFilename;
char *audioFilename;
char *outputFilename;
char *pin;


/*
 * These functions prints help message on the screen.
 */
void printEncodeUsage()
{
    printf("Usage: wavstego <options>\n\n");
    printf("Options for encode:\n");
    printf(" -e <pin>      encode mode with pin set\n");
    printf(" -m <filename> message file to hide\n");
    printf(" -a <filename> audio file\n");
    printf(" -o <filename> output audio file\n\n");
}
void printDecodeUsage()
{
    printf("Usage: wavstego <options>\n\n");
    printf("Options for decode:\n");
    printf(" -d <pin>      decode mode with pin set\n");
    printf(" -a <filename> audio file (output file will remain\n");
    printf("                           its original name)\n");
}
void printUsage()
{
    printEncodeUsage();
    printDecodeUsage();
}

/*
 * Extract commands from arguments that is passed to the main function.
 */
int parseCmd(int argc, char const *argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-d") == 0)
        {
            isEncode = false;
            pin      = argv[++i];
        }
        else if (strcmp(argv[i], "-e") == 0)
        {
            isEncode = true;
            pin      = argv[++i];
        }
        else if (strcmp(argv[i], "-m") == 0)
            msgFilename = argv[++i];
        else if (strcmp(argv[i], "-a") == 0)
            audioFilename = argv[++i];
        else if (strcmp(argv[i], "-o") == 0)
            outputFilename = argv[++i];
        else
        {
            printf("Unknown command.\n");
            return -1;
        }
    }
    if (isEncode)
    {
        if (pin == NULL || msgFilename == NULL || audioFilename == NULL || outputFilename == NULL)
        {
            printf("Missing arguments.\n");
            printEncodeUsage();
            return -1;
        }
    }
    else
    {
        if (pin == NULL || audioFilename == NULL)
        {
            printf("Missing arguments\n");
            printDecodeUsage();
            return -1;
        }
    }
    return 0;
}

/*
 * Get and return the length in bytes how long an assigned file is.
 * Return 0 if the file does not exist or failed to open.
 */
int getFileSize(char const *filename)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL)
        return 0;
    fseek(file, 0, SEEK_END);
    int size = (int)ftell(file);
    fclose(file);
    return size;
}

/*
 * Open the assigned file and read all the bytes into 'buffer'.
 */
int openMsgFile(char const *filename, int length, const char *buffer, int offset)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL)
        return -1;

    fread(buffer + offset, sizeof(char), length, file);
    fclose(file);
    return 0;
}

/*
 * Open audio file and return a pointer.
 * Return value can be viewed as an array of pointers which point to
 * the starting sample in each channel.
 */
double **openAudioFile(char const *filename, WAVE_INFO *wave_info)
{
    char tempTemp[] = "tmpXXXXXX";
    char *tempname;
    int r = open_wave(filename, wave_info);
    if (r != 0)
    {
        int fd;
        fd = mkstemp(tempTemp);
        close(fd);
        remove(tempTemp);
        tempname = (char *)malloc(13);
        memcpy(tempname, tempTemp, 9);
        tempname[9]  = '.';
        tempname[10] = 'w';
        tempname[12] = 'v';
        tempname[11] = 'a';
        pid_t pid = fork();
        if (pid == 0)
        { /* child process */
            static char *argv[] = {"ffmpeg", "-i", NULL, "-acodec", "pcm_s16le", NULL, NULL};
            argv[2]             = filename;
            argv[5] = tempname;
            execv("utils/ffmpeg", argv);
            exit(127); /* only if execv fails */
        }
        else
        {                       /* pid!=0; parent process */
            waitpid(pid, 0, 0); /* wait for child to exit */
        }
        int r = open_wave(tempname, wave_info);
        if (r != 0)
            return NULL;
    }
    double **temp = wave_read(wave_info, 0);
    remove(tempname);
    return temp;
}

/*
 * Readjust the sample data so that they are in [-1,1] range
 */
void normalize(double **data, const WAVE_INFO *wave_info)
{
    if (wave_info->bitDepth == 32)
    {
        double max = 0;
        for (int i = 0; i < wave_info->dataSize / (wave_info->bitDepth / 8); i++)
        {
            if (data[i % wave_info->channels][i / wave_info->channels] > max)
                max = data[i % wave_info->channels][i / wave_info->channels];
        }
        if (max < 1)
            return;
        else
            for (int i = 0; i < wave_info->dataSize / (wave_info->bitDepth / 8); i++)
                data[i % wave_info->channels][i / wave_info->channels] /= max;
    }
}

/*
 * Encode process
 *
 * Open message/file
 * Open audio file
 * Compress message/file
 * Encrypt message/file
 * Stego message/file into audio
 */
int encodeCycle()
{
    int size           = getFileSize(msgFilename);
    int filenameLength = strlen(msgFilename);
    char *msg          = (char *)malloc(size + filenameLength + sizeof(int) * 2);

    printf("Encode cycle started\n");

    // 4 bytes - filename string length / 4 bytes - Message length
    memcpy(msg, &filenameLength, sizeof(int));
    // undefinite - filename itself / indefinite - message itself
    memcpy(msg + sizeof(int), msgFilename, filenameLength);
    // 4 bytes - length of message / 4 byte - no message file indicator
    memcpy(msg + sizeof(int) + filenameLength, &size, sizeof(int));

    // If a file is assigned to stego, open it.
    if (size != 0)
        if (openMsgFile(msgFilename, size, msg, filenameLength + sizeof(int) * 2) == -1)
        {
            printf("Failed to open the assigned file.\n");
            free(msg);
            return -1;
        }

    printf("Message/File prepared.\n");

    WAVE_INFO wave_info;

    double **audio = openAudioFile(audioFilename, &wave_info);
    if (audio == NULL)
    {
        printf("Failed to open the assigned audio file.\n");
        free(msg);
        return -1;
    }

    printf("Audio file opened.\n");

    normalize(audio, &wave_info);

    int msglen = size + filenameLength + sizeof(int) * 2;
    int temp   = msglen; // Used to compute compression ratio.
    msglen = m_compress(&msg, msglen);
    if (msg == NULL || msglen <= 0)
    {
        printf("Failed to compress the message/file.\n");
        free(audio);
        return -1;
    }
    // printf("%d %d\n", temp, msglen);
    printf("Compression finished. (ratio: %.1f%%)\n", (double)msglen * 100 / temp);

    if (strlen(pin) > 16)
    {
        printf("Only 16 characters is allowed.\n");
        free(audio);
        return -1;
    }
    msglen = m_encrypt(&msg, msglen, pin, strlen(pin));
    // printf("%d\n", msglen);
    if (msg == NULL)
    {
        printf("Failed to encrypt the message/file.\n");
        free(audio);
        return -1;
    }
    printf("Encryption finished.\n");

    // printf("%d\n", wave_info.dataSize);
    if (stego(msg, msglen, audio, &wave_info, outputFilename) == -1)
    {
        printf("Stego failed.\n");
        free(msg);
        free(audio);
        return -1;
    }
    printf("Stego finished.\n");

    free(msg);
    free(audio);

    return 0;
}

/*
 * Decode process
 *
 * Open audio file
 * Destego to get a bitstream
 * Decrypt bitstream
 * Decompress bitstream
 * Identify it is a message or a file
 * Message: Print to the screen; File: Write to the disk
 */
int decodeCycle()
{
    printf("Decode cycle started.\n");
    char *msgFlow = destego(audioFilename);
    if (msgFlow == NULL)
    {
        return -1;
    }

    int len = 0;
    memcpy(&len, msgFlow, sizeof(int));
    char *msg = (char *)malloc(len);
    memcpy(msg, msgFlow + sizeof(int), len);
    free(msgFlow);

    if (strlen(pin) > 16)
    {
        printf("Only 16 characters is allowed.\n");
        return -1;
    }
    len = decrypt(&msg, len, pin, strlen(pin));
    if (msg == NULL)
    {
        printf("Failed to decrypt the message/file.\n");
        return -1;
    }
    printf("Decryption finished.\n");

    len = m_decompress(&msg, len);
    if (msg == NULL || len <= 0)
    {
        printf("Failed to decompress the message/file.\n");
        return -1;
    }
    printf("Decompression finished.\n");

    int nameOrMsgLength = 0;
    memcpy(&nameOrMsgLength, msg, sizeof(int));
    char *filenameOrMsg = (char *)malloc(nameOrMsgLength + 1);
    memcpy(filenameOrMsg, msg + sizeof(int), nameOrMsgLength);
    memset(filenameOrMsg + nameOrMsgLength, 0, 1);
    int msgLength = 0;
    memcpy(&msgLength, msg + sizeof(int) + nameOrMsgLength, sizeof(int));

    if (msgLength == 0)
    {
        printf("\nThe message contained in the file is: \n%s\n\n", filenameOrMsg);
    }
    else
    {
        char *bitstream = (char *)malloc(msgLength);
        memcpy(bitstream, msg + sizeof(int) * 2 + nameOrMsgLength, msgLength);
        int filenameindex = 0;
        for (filenameindex = nameOrMsgLength - 1; filenameindex > 0; filenameindex--)
            if (filenameOrMsg[filenameindex] == '/')
            {
                filenameindex++;
                break;
            }
        FILE *file = fopen(filenameOrMsg + filenameindex, "w");
        if (!file)
        {
            printf("Failed to open the file for writing.\n");
            free(msg);
            free(filenameOrMsg);
            return -1;
        }
        if (fwrite(bitstream, sizeof(char), msgLength, file) != msgLength)
        {
            printf("Failed to write the file.\n");
            free(msg);
            free(filenameOrMsg);
            fclose(file);
            return -1;
        }
        fclose(file);
        free(bitstream);
        printf("A file(%s) is extracted.\n", filenameOrMsg + filenameindex);
    }

    free(msg);
    free(filenameOrMsg);
    return 0;
}

int main(int argc, char const *argv[])
{
    if (parseCmd(argc, argv) == -1)
    {
        printf("Parse command failed.\n");
        return -1;
    }
    if (isEncode)
    {
        if (encodeCycle() == -1)
        {
            printf("Encode failed.\n");
            return -1;
        }
        else
        {
            printf("Encode cycle finished successfully.\n");
        }
    }
    else
    {
        if (decodeCycle() == -1)
        {
            printf("Decode failed.\n");
            return -1;
        }
        else
        {
            printf("Decode cycle finished successfully\n");
        }
    }
    return 0;
}
