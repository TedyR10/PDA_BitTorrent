**Name: Theodor-Ioan Rolea**

**Group: 333CA**

# HW3 APD - BitTorrent protocol

## Overview

* This project implements a file-sharing system similar to `BitTorrent` using
`MPI`. The system consists of a `tracker` and multiple `peers`. The tracker
manages file information, processes client requests, updates, and finalization,
ensuring synchronization among peers. Peers use threads for download and upload
operations. I will discuss the code structure, program logic, and final
thoughts, but in-depth comments are found throughout the code.

***

## Code Structure

### 1. Tracker
* The tracker is initialized with a list of swarms, one for each file, keeping
track of the number of seeders for one specific file and information about each
chunk of the file. Information about a chunk consists of the number of seeders
that have the respective chunk and the hash associated with the chunk. After
initialization, the tracker awaits requests from peers and processes them
accordingly. Once all the peers have finished downloading, the tracker
sends a shutdown signal to all peers and shuts down.

### 2. Peer

* The peer starts by reading the files it has and the files it wants to
download. Then, it starts two threads, one for download and one for upload. 
- In the download thread, the peer sends a request to the tracker for the files
it wants to download. The tracker responds with the list of peers that have the
respective file. The client then sends a request to each peer in the list for the
chunks it wants to download using a Round Robin approach. The peer receives
the chunks and updates the tracker every 10 chunks. Once the peer has finished
downloading, it sends a finalize signal to the tracker and shuts down the
download thread.
- In the upload thread, the peer awaits requests for chunks from other peers.
It will continue to send/receive chunks until it receives a shutdown signal
from the tracker.

***

## Final Thoughts

* This project was a great opportunity to learn about the BitTorrent protocol
and how it works under the hood. It also helped me deepen my knowledge of
MPI and how to use it to implement a distributed system. I had a lot of fun
working on this project. The most challenging part was to make sure that the
peers and the tracker have the correct information about the chunks. The labs
provided good examples that helped me get through the problems I encountered.