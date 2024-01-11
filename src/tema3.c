#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 33
#define MAX_CHUNKS 100
#define MAX_CLIENTS 100
#define UPDATE_TAG 21
#define FINALIZE_TAG 22
#define ALL_FINALIZED_TAG 23
#define FILE_REQUEST_TAG 24
#define FILE_RESPONSE_TAG 25
#define CHUNK_REQUEST_TAG 26
#define CHUNK_RESPONSE_TAG 27
#define DONE_TAG 28


typedef struct {
    char filename[MAX_FILENAME];
    int num_segments;
    char segments[MAX_CHUNKS][HASH_SIZE];
} File;

typedef struct {
    int num_seeders;
    int seeders[MAX_CLIENTS];
    char hash[HASH_SIZE];
} Chunk;

typedef struct {
    int num_files_owned;
    File owned_files[MAX_FILES];
    int num_files_requested;
    char requested_files[MAX_FILES][MAX_FILENAME];
} ClientInfo; 

typedef struct {
    char filename[MAX_FILENAME];
    int num_chunks;
    Chunk chunks[MAX_CHUNKS];
    int num_seeders;
    int seeders[MAX_CLIENTS];
} Swarm;

typedef struct {
    char filename[MAX_FILENAME];
    int chunk_index;
} UpdateClient;

typedef struct {
    int rank;
    ClientInfo clientInfo;
} ThreadArgs;

typedef struct {
    int rank;
    char hash[HASH_SIZE];
} ChunkRequestArgs;

void *download_thread_func(void *arg)
{
    ThreadArgs *threadArgs = (ThreadArgs*) arg;
    int rank = threadArgs->rank;
    ClientInfo clientInfo = threadArgs->clientInfo;

    // Transmit file information to the tracker
    MPI_Send(&clientInfo, sizeof(ClientInfo), MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD);
    
    // Wait for acknowledgment from the tracker
    char ack[3];
    MPI_Recv(ack, 3, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    while (1) {
        for (int i = 0; i < clientInfo.num_files_requested; i++) {    
            // Send file request to the tracker
            MPI_Send(&rank, 1, MPI_INT, TRACKER_RANK, FILE_REQUEST_TAG, MPI_COMM_WORLD);

            MPI_Send(clientInfo.requested_files[i], MAX_FILENAME, MPI_CHAR, TRACKER_RANK, FILE_REQUEST_TAG, MPI_COMM_WORLD);

            // Wait for a response from the tracker before starting download
            Swarm swarm;
            MPI_Recv(&swarm, sizeof(Swarm), MPI_BYTE, TRACKER_RANK, FILE_RESPONSE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            
            if (swarm.num_chunks == 0) {
                // File not found
                continue;
            }

            // Check if the client already has parts of the file
            int startChunk = 0, fileNumber = -1;
            for (int j = 0; j < clientInfo.num_files_owned; j++) {
                if (strcmp(clientInfo.owned_files[j].filename, clientInfo.requested_files[i]) == 0) {
                    startChunk = clientInfo.owned_files[j].num_segments;
                    fileNumber = j;
                    break;
                }
            }

            int chunksSent = 0;
            UpdateClient updateClient;
            strcpy(updateClient.filename, clientInfo.requested_files[i]);
            updateClient.chunk_index = startChunk;

            // Update client information
            if (fileNumber == -1) {
                // Client does not own the file
                fileNumber = clientInfo.num_files_owned;
                clientInfo.num_files_owned++;
                strcpy(clientInfo.owned_files[fileNumber].filename, clientInfo.requested_files[i]);
            }

            // Start download using a Round Robin approach
            for (int j = startChunk; j < swarm.num_chunks; j++) {
                // Find the peer that has the chunk using Round Robin
                // j will increase, so we will have a different peer for each
                // chunk in a circular manner
                int peerRank = j % swarm.chunks[j].num_seeders;

                if (swarm.chunks[j].seeders[peerRank] == 0) {
                    // Break if no peer has the chunk
                    break;
                }
                
                // Send chunk request to a peer
                ChunkRequestArgs chunkRequestArgs;
                chunkRequestArgs.rank = rank;
                strcpy(chunkRequestArgs.hash, swarm.chunks[j].hash);

                MPI_Send(&chunkRequestArgs, sizeof(ChunkRequestArgs), MPI_BYTE, swarm.chunks[j].seeders[peerRank], CHUNK_REQUEST_TAG, MPI_COMM_WORLD);
                
                // Receive ACK from a peer
                char ack[3];
                MPI_Recv(ack, 3, MPI_CHAR, swarm.chunks[j].seeders[peerRank], CHUNK_RESPONSE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                // Update client information
                strcpy(clientInfo.owned_files[fileNumber].segments[j], swarm.chunks[j].hash);

                // Update chunks sent
                chunksSent++;

                // If 10 chunks have been sent, send an update to the tracker
                // Omit sending an update for the last chunk
                if (chunksSent == 10 && j != swarm.num_chunks - 2) {
                    // Send update to the tracker
                    MPI_Send(&rank, 1, MPI_INT, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD);

                    MPI_Send(&updateClient, sizeof(updateClient), MPI_BYTE, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD);

                    // Update the chunk index
                    updateClient.chunk_index += 10;
                    chunksSent = 0;

                    // Wait for a response from the tracker before continuing
                    MPI_Recv(&swarm, sizeof(Swarm), MPI_BYTE, TRACKER_RANK, FILE_RESPONSE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                }
            }

            // Send final update to the tracker
            if (chunksSent != 0) {
                MPI_Send(&rank, 1, MPI_INT, TRACKER_RANK, FINALIZE_TAG, MPI_COMM_WORLD);
                MPI_Send(&updateClient, sizeof(updateClient), MPI_BYTE, TRACKER_RANK, FINALIZE_TAG, MPI_COMM_WORLD);
            }

            // Update chunk number
            clientInfo.owned_files[fileNumber].num_segments = swarm.num_chunks;

            // Save downloaded file
            char outputFileName[40];
            snprintf(outputFileName, sizeof(outputFileName), "client%d_%s", rank, clientInfo.requested_files[i]);

            FILE *fileList = fopen(outputFileName, "a");
            if (fileList == NULL) {
                perror("Error opening input file");
                exit(EXIT_FAILURE);
            }
            for (int k = 0; k < swarm.num_chunks; k++) {
                // Copy the contents of the chunk file to the input file
                fprintf(fileList, "%s\n", swarm.chunks[k].hash);
            }
            fclose(fileList);
        }

        // Send update to the tracker that all downloads are done
        MPI_Send(&rank, 1, MPI_INT, TRACKER_RANK, DONE_TAG, MPI_COMM_WORLD);
        break;
    }

    return NULL;
}

void *upload_thread_func(void *arg)
{
    
    while (1) {
        // Wait for a request from a peer
        ChunkRequestArgs chunkRequestArgs;
        MPI_Recv(&chunkRequestArgs, sizeof(ChunkRequestArgs), MPI_BYTE, MPI_ANY_SOURCE, CHUNK_REQUEST_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        if (strcmp(chunkRequestArgs.hash, "FINISH") == 0) {
            // Break if all the other peers have finished downloading
            break;
        }

        if (chunkRequestArgs.hash == NULL) {
            // Break if an invalid hash is received
            break;
        }

        // Send ACK to the peer
        MPI_Send("ACK", 3, MPI_CHAR, chunkRequestArgs.rank, CHUNK_RESPONSE_TAG, MPI_COMM_WORLD);
    }

    return NULL;
}

// Function to find the index of a file in swarms
int findFileIndex(Swarm *swarms, const char *filename, int numFiles) {
    for (int i = 0; i < numFiles; i++) {
        if (swarms[i].filename != NULL && filename != NULL && strcmp(swarms[i].filename, filename) == 0) {
            return i;
        }
    }
    return -1;
}

// Function to handle client update
void handleClientUpdate(int clientRank, Swarm *swarms, int numFiles) {
    // Receive update message from client
    UpdateClient updateClient;
    MPI_Recv(&updateClient, sizeof(updateClient), MPI_BYTE, clientRank, UPDATE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // Find the file in swarms
    int swarmIndex = findFileIndex(swarms, updateClient.filename, numFiles);

    // Update swarms
    for (int i = updateClient.chunk_index; i < updateClient.chunk_index + 10; i++) {
        if (swarmIndex != -1 && i < swarms[swarmIndex].num_chunks) {
            // Mark the client as a seeder for the chunk
            swarms[swarmIndex].chunks[i].seeders[swarms[swarmIndex].chunks[i].num_seeders] = clientRank;
            swarms[swarmIndex].chunks[i].num_seeders++;
        }
    }

    // Send swarm information back to the client
    MPI_Send(&swarms[swarmIndex], sizeof(Swarm), MPI_BYTE, clientRank, FILE_RESPONSE_TAG, MPI_COMM_WORLD);
}

// Function to handle client finalization
void handleClientFinalization(int clientRank, Swarm *swarms, int numFiles) {
    // Receive update message from client
    UpdateClient updateClient;
    MPI_Recv(&updateClient, sizeof(updateClient), MPI_BYTE, clientRank, FINALIZE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // Find the file in swarms
    int swarmIndex = findFileIndex(swarms, updateClient.filename, numFiles);

    // Update swarms
    for (int i = updateClient.chunk_index; i < swarms[swarmIndex].num_chunks; i++) {
        if (swarmIndex != -1 && i < swarms[swarmIndex].num_chunks) {
            // Mark the client as a seeder for the chunk
            swarms[swarmIndex].chunks[i].seeders[swarms[swarmIndex].chunks[i].num_seeders] = clientRank;
            swarms[swarmIndex].chunks[i].num_seeders++;
        }
    }

    // Mark the client as a seeder for the file
    swarms[swarmIndex].seeders[swarms[swarmIndex].num_seeders] = clientRank;
    swarms[swarmIndex].num_seeders++;
}

// Function to handle all clients finalized message
void handleAllClientsFinished(int numtasks) {
    ChunkRequestArgs chunkRequestArgs;
    strcpy(chunkRequestArgs.hash, "FINISH");

    // Send acknowledgment to all clients that they can finish
    for (int i = 1; i < numtasks; i++) {
        chunkRequestArgs.rank = i;
        MPI_Send(&chunkRequestArgs, sizeof(ChunkRequestArgs), MPI_BYTE, i, CHUNK_REQUEST_TAG, MPI_COMM_WORLD);
    }
}

// Function to handle file request from a client
void handleFileRequest(int clientRank, Swarm *swarms, int numFiles) {
    // Receive file request message from client
    char filename[MAX_FILENAME];
    MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, clientRank, FILE_REQUEST_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // Find the file in swarms and send information back to the client
    int fileIndex = findFileIndex(swarms, filename, numFiles);
    if (fileIndex != -1) {
        // Send file information to the client
        MPI_Send(&swarms[fileIndex], sizeof(Swarm), MPI_BYTE, clientRank, FILE_RESPONSE_TAG, MPI_COMM_WORLD);
    } else {
        // Send a message indicating file not found
        MPI_Send("FILE_NOT_FOUND", 15, MPI_CHAR, clientRank, FILE_RESPONSE_TAG, MPI_COMM_WORLD);
    }
}

void tracker(int numtasks, int rank) {
    Swarm swarms[MAX_FILES];
    int numFiles = 0;

    MPI_Status status;
    int source, tag;

    int finishedClients = 0;

    for (int i = 1; i < numtasks; i++) {
        // Receive initialization message from each client
        ClientInfo clientInfo;
        
        MPI_Recv(&clientInfo, sizeof(ClientInfo), MPI_BYTE, i, 0, MPI_COMM_WORLD, &status);
        
        // Update swarms based on client information
        for (int j = 0; j < clientInfo.num_files_owned; j++) {
            int swarmIndex = findFileIndex(swarms, clientInfo.owned_files[j].filename, numFiles);
            if (swarmIndex == -1) {
                // File not found in swarm, add it
                strcpy(swarms[numFiles].filename, clientInfo.owned_files[j].filename);
                swarms[numFiles].num_chunks = clientInfo.owned_files[j].num_segments;
                
                // Add chunks to the swarm
                for (int k = 0; k < swarms[numFiles].num_chunks; k++) {
                    swarms[numFiles].chunks[k].num_seeders = 0;
                    swarms[numFiles].chunks[k].seeders[swarms[numFiles].chunks[k].num_seeders] = i;
                    strcpy(swarms[numFiles].chunks[k].hash, clientInfo.owned_files[j].segments[k]);
                    swarms[numFiles].chunks[k].num_seeders++;
                }
                
                // Add client to the list of seeders for the file
                swarms[numFiles].num_seeders = 0;
                swarms[numFiles].seeders[swarms[numFiles].num_seeders] = i;
                swarms[numFiles].num_seeders++;

                numFiles++;
            } else {
                
                // File found in swarm, update it
                for (int k = 0; k < clientInfo.owned_files[j].num_segments; k++) {
                    swarms[swarmIndex].chunks[k].seeders[swarms[swarmIndex].chunks[k].num_seeders] = i;
                    swarms[swarmIndex].chunks[k].num_seeders++;
                }

                // Add client to the list of seeders for the file
                swarms[swarmIndex].seeders[swarms[swarmIndex].num_seeders] = i;
                swarms[swarmIndex].num_seeders++;
            }
        }

        // Send acknowledgment to the client
        MPI_Send("ACK", 3, MPI_CHAR, i, 0, MPI_COMM_WORLD);
    }

    // Handle ongoing communication with clients
    while (1) {
        // Receive messages from clients
        MPI_Recv(&source, 1, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        tag = status.MPI_TAG;

        if (tag == FILE_REQUEST_TAG) {
            // Handle file request from a client
            handleFileRequest(source, swarms, numFiles);
        } else if (tag == UPDATE_TAG) {
            // Handle update from a client
            handleClientUpdate(source, swarms, numFiles);
        } else if (tag == FINALIZE_TAG) {
            // Handle client finalization
            handleClientFinalization(source, swarms, numFiles);
        } else if (tag == DONE_TAG) {
            // Update the number of clients that have finished
            finishedClients++;
        }

        // Check if all clients have finished
        if (finishedClients == numtasks - 1) {
            // Handle all clients finalized
            handleAllClientsFinished(numtasks);
            break;
        }
    }
}

// Function to initialize a peer
ClientInfo initializePeer(int rank) {
    // Construct the input file name based on the rank
    char inputFileName[40];
    snprintf(inputFileName, sizeof(inputFileName), "in%d.txt", rank);

    // Open the input file
    FILE *fileList = fopen(inputFileName, "r");
    if (fileList == NULL) {
        perror("Error opening input file");
        exit(EXIT_FAILURE);
    }

    // Initialize the clientInfo structure
    ClientInfo clientInfo;
    fscanf(fileList, "%d\n", &clientInfo.num_files_owned);

    char line[BUFSIZ];

    for (int i = 0; i < clientInfo.num_files_owned; i++) {
        // Read file details
        fscanf(fileList, "%s %d\n", clientInfo.owned_files[i].filename, &clientInfo.owned_files[i].num_segments);

        // Read segments
        for (int j = 0; j < clientInfo.owned_files[i].num_segments; j++) {
			fgets(line, BUFSIZ, fileList);
            if (line[strlen(line) - 1] == '\n')
			    line[strlen(line) - 1] = '\0';
			strcpy(clientInfo.owned_files[i].segments[j], line);
        }
    }

    // Read the number of requested files
    fgets(line, BUFSIZ, fileList);
    if (line[strlen(line) - 1] == '\n')
        line[strlen(line) - 1] = '\0';
    clientInfo.num_files_requested = atoi(line);

    for (int i = 0; i < clientInfo.num_files_requested; i++) {
        // Read file details
        fgets(clientInfo.requested_files[i], MAX_FILENAME, fileList);
        if (clientInfo.requested_files[i][strlen(clientInfo.requested_files[i]) - 1] == '\n')
            clientInfo.requested_files[i][strlen(clientInfo.requested_files[i]) - 1] = '\0';
    }

    fclose(fileList);
    return clientInfo;
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    ThreadArgs threadArgs;
    threadArgs.rank = rank;
    threadArgs.clientInfo = initializePeer(rank);

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &threadArgs);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
