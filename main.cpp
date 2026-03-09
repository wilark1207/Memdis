#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <ostream>
#include <sstream>
#include <string>
#include <sys/event.h> // MAC ONLY: The epoll (linux) alternative (kqueue)
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// LINUX OHLY: #include <sys/epoll.h>

#define MAX_EVENTS 100

// Helper to make a socket Non-Blocking
void set_non_blocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

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
  struct sockaddr_in address;
  int opt = 1;
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

  set_non_blocking(server_fd);

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
  if (listen(server_fd, 511) < 0) {
    std::cerr << "Listen failed" << std::endl;
    return 1;
  }

  std::cout << " Memdis Engine V0.3 (Kqueue) running on port: " << port << "..."
            << std::endl;

  // --------------------------------------------------------------
  // 1. CREATE THE QUEUE
  // MAC: kqueue()
  // LINUX: epoll_create1(0)
  // --------------------------------------------------------------
  int kq = kqueue();
  if (kq == -1) {
    std::cerr << "Failed to create kqueue" << std::endl;
    return 1;
  }

  // --------------------------------------------------------------
  // 2. REGISTER THE GREETER (server_fd)
  // MAC: EV_SET configures the struct, kevent() registers it.
  // LINUX: event.events = EPOLLIN; epoll_ctl(epoll_fd, EPOLL_CTL_ADD,
  // server_fd, &event)
  // --------------------------------------------------------------
  struct kevent change_event;
  // We want to READ (EVFILT_READ), we are ADDING it (EV_ADD), and ENABLING it
  // (EV_ENABLE)
  EV_SET(&change_event, server_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
  kevent(kq, &change_event, 1, NULL, 0, NULL);

  // Array to hold the "Ready List" that the OS hands back
  // MAC: struct kevent
  // LINUX: struct epoll_event
  struct kevent event_list[MAX_EVENTS];

  while (true) {

    // --------------------------------------------------------------
    // 2. REGISTER THE GREETER (server_fd)
    // MAC: EV_SET configures the struct, kevent() registers it.
    // LINUX: event.events = EPOLLIN; epoll_ctl(epoll_fd, EPOLL_CTL_ADD,
    // server_fd, &event)
    // --------------------------------------------------------------
    int num_ready = kevent(kq, NULL, 0, event_list, MAX_EVENTS, NULL);

    if (num_ready < 0) {
      std::cerr << "kevent error" << std::endl;
      break;
    }

    // only loop exactly 'num_ready' times. O(K) complexity
    for (int i{}; i < num_ready; ++i) {
      // MAC uses .ident to get the file descriptor
      // LINUX uses .data.fd
      int current_fd = event_list[i].ident;

      // Scenario A: The Greeter's socket has an event (New connection)
      if (current_fd == server_fd) {
        int new_socket;
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        new_socket = accept(server_fd, (struct sockaddr *)&client_addr,
                            &client_addr_len);
        if (new_socket < 0)
          continue;

        set_non_blocking(new_socket);

        // Add new client to kqeuue
        // LINUX: epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_socket, &client_event)
        struct kevent client_event;
        EV_SET(&client_event, new_socket, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
               NULL);
        kevent(kq, &client_event, 1, NULL, 0, NULL);

        std::cout << "[LOG] Client connected! FD: " << new_socket << std::endl;
      } else {
        // Scenario B: Existing client sent us a command
        // MAC kqueue has a nice feature: it tells if the client disconnected
        // via flags LINUX: rely entirely on read() returning 0
        if (event_list[i].flags & EV_EOF) {
          std::cout << "[LOG] Client disconnected (EOF). FD: " << current_fd
                    << std::endl;
          close(current_fd);
          continue;
        }

        char buffer[1024] = {0};
        int bytes_read = read(current_fd, buffer, sizeof(buffer));

        if (bytes_read <= 0) {
          std::cout << "[LOG] Client disconnected (Read 0). FD: " << current_fd
                    << std::endl;
          close(current_fd);
        } else {
          // --- THE BRAIN OF THE DATABASE ---
          std::string raw_command(buffer);
          auto tokens = split_command(raw_command);
          std::string response = "-ERR Unknown Command\n";

          if (!tokens.empty()) {
            std::string cmd = tokens[0];
            if (cmd == "SET" && tokens.size() >= 3) {
              std::string key = tokens[1];
              std::string value = "";
              for (size_t j = 2; j < tokens.size(); ++j) {
                value += tokens[j] + (j == tokens.size() - 1 ? "" : " ");
              }
              datastore[key] = value;
              response = "+OK\n";
              std::cout << "[LOG] SET " << key << std::endl;
            } else if (cmd == "GET" && tokens.size() == 2) {
              std::string key = tokens[1];
              if (datastore.find(key) != datastore.end()) {
                response = "$" + std::to_string(datastore[key].length()) +
                           "\n" + datastore[key] + "\n";
              } else {
                response = "$-1\n";
              }
              std::cout << "[LOG] GET " << key << std::endl;
            }
          }
          send(current_fd, response.c_str(), response.length(), 0);
        }
      }
    }
  }

  close(server_fd);
  return 0;
}
}
}
}

return 0;
}
