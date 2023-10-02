# Computer Networking coursework project

This repository contains the coursework project for a P2P Pandemic Tracking Application. It's a tool designed to monitor and analyze through simple distributed queries the progression of pandemics in a decentralized manner.

Developed in **C language**, uses **I/O multiplexing** to handle the communication with multiple peers and handle the user input (for inserting data and performing queries).
Also uses the standard C library **wordexp** to for shell-like expansion of user input.

## Features
- Hospital operators can input pandemic-related data (eg. number of cases) into their local peer archives
- Each peer independently manages its local data for data decentralization
- Supports various types of data queries requiring data aggregation from peers
- Peers exchange data through query-flooding to respond to queries
- Implements a basic caching mechanism to optimize query response times for frequently requested results

## Structure
```
├─ Modules*:
│   ├─ comm             # msg struct + functions to send/receive
│   ├─ commandline      # functions to read from command line 
│   ├─ common_utils     # common utility functions
│   ├─ data             # represents and handles a peer's local archive
│   └── graph           # represents a local graph of the peers
├─ ds                   # (main) discovery server
└── peer                # (main) peer
```
*Each module is a pair source+header.

## How to test
First, compile with `make all`, then
1. Run the discovery server with `./ds 4242`
2. Run at least three peers with `./peer <PeerPort>`
3. Type `help` to see the commands available.

### Examples
- Run `get sum t 2021:09:08-2021:09:11` on a peer
- Run `get var c` on a peer
- RUn `showpeers` on the discovery server

### Notes
The current date is fixed to "2021-09-14" in `constants.h` for testing purposes since the initial testing dataset is fixed.

## Author
Paolo Salvatore Galfano ([paolosalvatore.galfano@mail.polimi.it](mailto:paolosalvatore.galfano@mail.polimi.it))

