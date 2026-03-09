#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <ostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// The V0.1 In-Memory Datastore
std::unordered_map<std::string, std::string> datastore;

// Helper to split incoming text commands (e.g., "SET user:123 Hello")
std::vector<std::string> split_command(const std::string &str) {
  std::vector<std::string> tokens;
  std::stringstream ss(str);
  std::string token;
  while (ss >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

int main() {
  // `fd` - File Descriptor
  // When socket() is called, the OS creates the network resource and hands the
  // C++ program a siple integer (like 3 or 4). That integer is the "ticket" to
  // use the socket
  int server_fd;
  int new_socket;
  struct sockaddr_in address;
  int opt = 1;
  int addrlen = sizeof(address);
  int port = 6380; // Custom port for our engine

  // 1. Create socket file descriptor
  // Asks the OS kernel to create an endpoint for communication
  // SOCK_STREAM is TCP (reliable, ordered packets)
  // SOCK_DGRAM is UDP (fast, unreliable packets, used in gaming/HFT)
  // if 0 is returned then OS denied our request
  if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    std::cerr << "Socket failed" << std::endl;
    return 1;
  }

  // 2. Attach socket to the port (prevents "Address already in use" errors)
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
    std::cerr << "Setsockopt failed" << std::endl;
    return 1;
  }

  address.sin_family = AF_INET;
  // listen on all network interfaces (Localhost, WiFi, Ethernet)
  address.sin_addr.s_addr = INADDR_ANY;
  // htons(port): Converts the port(6380) from CPU's byte order (Little-Endian)
  // to the Network's byte order (Big-Endien)
  address.sin_port = htons(port);

  // 3. Bind the socket to the port
  // officially locking down the port 6380 and binding it with the server_fd
  // ticket OS gave it
  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    std::cerr << "Bind failed" << std::endl;
    return 1;
  }

  // 4. Start listening for connections
  // OS starts accepting packets
  // 3 is the backlog - if 10 clients try to connect at the same millisecond,
  // OS will queue 3 of them and reject the rest until the code catches up
  // In production Redis, this backlog is usually set to 511 or higher
  if (listen(server_fd, 10) < 0) {
    std::cerr << "Listen failed" << std::endl;
    return 1;
  }

  std::cout << "--- Memdis Engine V0.2 (Event Loop) running on port ---" << port
            << "..." << std::endl;

  // Client Tracker
  // list to remember everyone who is connected
  std::vector<int> client_sockets;

  fd_set readfds;

  while (true) {

    // Step 1: clear the checklist
    FD_ZERO(&readfds);

    // Step 2: Add the Greeter (server_fd) to the checklist
    FD_SET(server_fd, &readfds);
    // select() needs to know the highest integer ID
    int max_sd = server_fd;

    // Step 3: Add all currently connected clients to the checklist
    for (int sd : client_sockets) {
      FD_SET(sd, &readfds);
      if (sd > max_sd) {
        max_sd = sd;
      }
    }

    // Step 4: System sleeps
    // The thread halts here. Costs 0% CPU
    // It wakes up the exact mirosecond any socket in the checklist has data
    int activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);

    if (activity < 0) {
      std::cerr << "Select Error" << std::endl;
    }

    // Step 5: New Client connecting
    if (FD_ISSET(server_fd, &readfds)) {
      if ((new_socket = accept(server_fd, (struct sockaddr *)&address,
                               (socklen_t *)&addrlen)) >= 0) {
        std::cout << "[LOG] New connection. Socket FD: " << new_socket
                  << std::endl;
        // add the new client to the tracking list
        client_sockets.emplace_back(new_socket);
      }
    }

    // Step 6: Loop thorugh the list of clients to see whose "box is ticked" by
    // the OS
    for (auto it = client_sockets.begin(); it != client_sockets.end();) {
      int sd = *it;

      if (FD_ISSET(sd, &readfds)) {
        char buffer[1024] = {0};
        int valread = read(sd, buffer, 1024);

        if (valread == 0) {
          // they disconnected
          std::cout << "[LOG] Client disconnected. Socket FD: " << sd
                    << std::endl;
          close(sd);

          // remove from the Tracker
          it = client_sockets.erase(it);
          continue;
        } else if (valread > 0) {
          // --- YOUR PARSING LOGIC STAYS EXACTLY THE SAME ---
          std::string raw_command(buffer);
          auto tokens = split_command(raw_command);
          std::string response = "-ERR Unknown Command\n";

          if (!tokens.empty()) {

            std::string cmd = tokens[0];
            if (cmd == "SET" && tokens.size() >= 3) {

              std::string key = tokens[1];
              std::string value = "";

              for (size_t i = 2; i < tokens.size(); ++i) {
                value += tokens[i] + (i == tokens.size() - 1 ? "" : " ");
              }

              datastore[key] = value;
              response = "+OK\n";

            } else if (cmd == "GET" && tokens.size() == 2) {

              std::string key = tokens[1];

              if (datastore.find(key) != datastore.end()) {
                response = "$" + std::to_string(datastore[key].length()) +
                           "\n" + datastore[key] + "\n";
              } else {
                response = "$-1\n";
              }
            }
          }
          // Send response back
          send(sd, response.c_str(), response.length(), 0);
        }
      }
      ++it;
    }
  }

  return 0;
}
