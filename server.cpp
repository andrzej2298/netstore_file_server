#include <iostream>
#include <list>
#include <regex>
#include <cassert>
#include <algorithm>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <csignal>
#include <netdb.h>
#include <fcntl.h>

#include "connection.h"

#define QUEUE_LENGTH 1

namespace po = boost::program_options;
namespace fs = boost::filesystem;

std::size_t MAX_SPACE_DEFAULT = 52428800;
std::size_t TIMEOUT_DEFAULT = 5;
std::size_t TIMEOUT_MAX = 300;

struct server_options;
struct server_state;

using file_infos = std::vector<fs::path>;

/**
 * Flags provided by the user.
 */
struct server_options {
    std::string MCAST_ADDR = "";
    int CMD_PORT = 0;
    std::size_t MAX_SPACE = 0;
    std::string SHRD_FLDR = "";
    unsigned int TIMEOUT = 0;
};

/**
 * Current server state.
 */
struct server_state {
    uint64_t available_space = 0; /** file storage available */
    uint64_t negative_space = 0; /** if after indexing the files their size is too big, the surplus number
                                  * of bytes is stored here */
    int socket = 0; /** UDP multicast socket */
    struct ip_mreq ip_mreq{}; /** info about the multicast group */
    file_infos files; /** list of files */
    std::set<std::string> open_files; /** created files that haven't yet been saved */
};

server_state current_server_state{};
int parent_pid = getpid();

void clean_up(server_state &state) {
    kill(-getpid(), SIGINT);

    if (getpid() == parent_pid) {
        /* dropping multicast group membership (only once) */
        if (setsockopt(state.socket, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                       (void *) &state.ip_mreq, sizeof state.ip_mreq) < 0) {
            throw std::runtime_error("setsockopt");
        }
    }
    close(state.socket);
    for (const std::string &filename : state.open_files) {
        unlink(filename.c_str());
    }
}

void catch_sigint(int) {
    clean_up(current_server_state);
    exit(-1);
}

void add_signal_handlers() {
    struct sigaction sigint_handler{};
    sigint_handler.sa_handler = catch_sigint;
    sigemptyset(&sigint_handler.sa_mask);
    sigint_handler.sa_flags = 0;

    /* handle CTRL+C */
    if (sigaction(SIGINT, &sigint_handler, nullptr) == -1) {
        throw std::runtime_error("sigaction");
    }

    struct sigaction sigchld_handler{};
    sigchld_handler.sa_handler = SIG_IGN;
    sigemptyset(&sigchld_handler.sa_mask);
    sigchld_handler.sa_flags = 0;

    /* handle an exit of a child */
    if (sigaction(SIGCHLD, &sigchld_handler, nullptr)) {
        throw std::runtime_error("sigaction");
    }
}

/**
 * Reads command line flags supplied by the user.
 * @param [in] argc Argument count.
 * @param [in] argv
 * @return Parsed options.
 */
server_options read_options(int argc, char const *argv[]) {
    po::options_description description("Allowed options");
    server_options options;

    description.add_options()
            ("help", "help message")
            ("mcast-addr,g", po::value<std::string>(&options.MCAST_ADDR))
            ("cmd-port,p", po::value<int>(&options.CMD_PORT))
            ("max-space,b", po::value<std::size_t>(&options.MAX_SPACE)->default_value(MAX_SPACE_DEFAULT))
            ("shrd-fldr,f", po::value<std::string>(&options.SHRD_FLDR))
            ("timeout,t", po::value<unsigned int>(&options.TIMEOUT)->default_value(TIMEOUT_DEFAULT));
    std::string mandatory_variables[] = {"mcast-addr", "cmd-port", "shrd-fldr"};

    po::variables_map variables;
    po::store(po::parse_command_line(argc, argv, description), variables);
    po::notify(variables);

    for (const std::string &variable: mandatory_variables) {
        if (!variables.count(variable)) {
            throw std::invalid_argument(variable);
        }
    }
    if (options.TIMEOUT > TIMEOUT_MAX || options.TIMEOUT == 0) {
        throw std::invalid_argument("timeout");
    }
    if (options.CMD_PORT < 0) {
        throw std::invalid_argument("port");
    }

    return options;
}

/**
 * Index files in @ref options.SHRD_FLDR.
 * @param [in] options
 * @param [out] state
 */
void index_files(const server_options &options, server_state &state) {
    fs::path dir_path(options.SHRD_FLDR);
    state.available_space = options.MAX_SPACE;

    if (fs::exists(dir_path) && fs::is_directory(dir_path)) {
        std::vector<fs::path> files;

        for (fs::directory_iterator it(dir_path); it != fs::directory_iterator(); ++it) {
            if (fs::is_regular_file(it->path())) {
                fs::path file_path = it->path();
                std::size_t current_file_size = file_size(file_path);
                state.files.push_back(file_path);
                if (state.available_space < current_file_size) {
                    current_file_size -= state.available_space;
                    state.available_space = 0;
                    state.negative_space += current_file_size;
                }
                else {
                    state.available_space -= current_file_size;
                }
            }
        }
    }
    else {
        throw std::invalid_argument("wrong directory");
    }
}

/** Initialize the UDP socket used to connect with the clients. */
void initialize_connection(const server_options &options, server_state &state) {
    struct sockaddr_in local_address{};

    /* opening socket */
    state.socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (state.socket < 0) {
        throw std::runtime_error("socket");
    }

    /* joining the multicast group */
    state.ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (inet_aton(options.MCAST_ADDR.c_str(), &state.ip_mreq.imr_multiaddr) == 0) {
        throw std::runtime_error("inet_aton");
    }

    if (setsockopt(state.socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (void *) &state.ip_mreq, sizeof state.ip_mreq) < 0) {
        throw std::runtime_error("setsockopt");
    }

    /* local address and port */
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons(options.CMD_PORT);
    if (bind(state.socket, (struct sockaddr *) &local_address, sizeof local_address) < 0) {
        throw std::runtime_error("bind");
    }
}

/** Check if the request has a valid command. */
bool command_equal(const SIMPL_CMD &request, const std::string &command) {
    for (std::size_t i = command.length(); i < request.cmd.length(); ++i) {
        if (request.cmd[i] != '\0') {
            return false;
        }
    }
    return request.cmd.substr(0, command.length()) == command;
}

/** Handle the clients "discover" message. */
void
discover(server_state &state, server_options &options, const struct sockaddr_in &client_address, SIMPL_CMD &request) {
    if (check_data_empty(request, client_address)) {
        send_complex_message(state.socket, client_address, "GOOD_DAY", options.MCAST_ADDR, request.cmd_seq,
                             state.available_space);
    }
}

/** Handle the clients "remove" message. */
void remove(server_state &state, const struct sockaddr_in &client_address, SIMPL_CMD &request) {
    if (check_data_not_empty(request, client_address)) {
        const std::string &target_file_name = request.data;
        auto it = state.files.begin();

        for (; (it != state.files.end()) && (it->filename().string() != target_file_name); ++it) {}
        if (it != state.files.end()) {
            std::size_t size = file_size(*it);
            if (state.negative_space > 0) {
                if (state.negative_space > size) {
                    state.negative_space -= size;
                }
                else {
                    size -= state.negative_space;
                    state.negative_space = 0;
                    state.available_space += size;
                }
            }
            else {
                state.available_space += size;
            }
            fs::remove(*it);
            state.files.erase(it);
        }
    }
}

/** Handle the clients "search" message. */
void list(server_state &state, const struct sockaddr_in &client_address, SIMPL_CMD &request) {
    std::string &target_file_name = request.data;
    std::list<std::string> results;
    for (const fs::path &file : state.files) {
        std::string file_name = file.filename().string();
        if (file_name.find(target_file_name) != std::string::npos) {
            results.push_back(file_name);
        }
    }

    std::string data;

    while (!results.empty()) {
        if (!results.empty()) {
            std::string current = results.front();
            data = current;
            results.pop_front();
            /* prepare one UDP packet */
            while (!results.empty() && data.size() + current.size() + 1 < MAX_SIMPL_DATA_LEN) {
                current = results.front();
                results.pop_front();
                data += "\n" + current;
            }
        }
        send_simple_message(state.socket, client_address, "MY_LIST", data, request.cmd_seq);
        data.clear();
    }
}

/** Creates a TCP socket used to transfer files between the client and the server. */
void
create_tcp_socket(int &sock, struct sockaddr_in &server_tcp,
                  socklen_t &server_tcp_len) {
    /* IPv4 TCP socket */
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        throw std::runtime_error("socket");
    }

    server_tcp.sin_family = AF_INET;
    server_tcp.sin_addr.s_addr = htonl(INADDR_ANY);
    /* ephemeral port */
    server_tcp.sin_port = htons(0);

    if (bind(sock, (struct sockaddr *) &server_tcp, server_tcp_len) < 0) {
        throw std::runtime_error("bind");
    }
    if (listen(sock, QUEUE_LENGTH) < 0) {
        throw std::runtime_error("listen");
    }

    /* get the port number */
    if (getsockname(sock, (struct sockaddr *) &server_tcp, &server_tcp_len) < 0) {
        throw std::runtime_error("getsockname");
    }
}

/** Handles the transfer of a file to the client. */
void send_file(server_options &options, server_state &state, const struct sockaddr_in &client_udp,
               SIMPL_CMD &request, const fs::path &path) {
    int sock, msg_sock;
    struct sockaddr_in server_tcp{};
    struct sockaddr_in client_tcp{};
    socklen_t client_tcp_len = sizeof client_tcp;
    socklen_t server_tcp_len = sizeof server_tcp;
    ssize_t snd_len;

    create_tcp_socket(sock, server_tcp, server_tcp_len);
    struct timeval wait_time{options.TIMEOUT, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    char buffer[BSIZE];

    send_complex_message(state.socket, client_udp, "CONNECT_ME", request.data, request.cmd_seq,
                         ntohs(server_tcp.sin_port));

    /* wait TIMEOUT to connect */
    if (select(sock + 1, &fds, nullptr, nullptr, &wait_time)) {
        client_tcp_len = sizeof(client_tcp);
        msg_sock = accept(sock, (struct sockaddr *) &client_tcp, &client_tcp_len);
        if (msg_sock < 0) {
            throw std::runtime_error("accept");
        }

        int fd, read_len;
        if ((fd = open(path.string().c_str(), O_RDONLY)) < 0) {
            throw std::runtime_error("open");
        }
        while ((read_len = read(fd, buffer, BSIZE)) > 0) {
            snd_len = write(msg_sock, buffer, read_len);
            if (snd_len != read_len) {
                throw std::runtime_error("writing to client socket");
            }
        }
        if (read_len < 0) {
            throw std::runtime_error("read");
        }
        if (close(msg_sock) < 0)
            throw std::runtime_error("close");
    }

    close(state.socket);
    close(sock);
    exit(0);
}

/** Handle the clients "fetch" message. */
void
fetch(server_options &options, server_state &state, const struct sockaddr_in &client_address, SIMPL_CMD &request) {
    for (const fs::path &file : state.files) {
        if (file.filename().string() == request.data) {
            switch (fork()) {
                case -1:
                    throw std::runtime_error("fork");
                case 0:
                    send_file(options, state, client_address, request, file);
                    break;
                default:
                    break;
            }
            return;
        }
    }
    error_message(client_address, "Invalid file name.");
}

/** Handles the transfer of a file from the client. */
void receive_file(server_options &options, server_state &state, const struct sockaddr_in &client_udp,
                  CMPLX_CMD &request) {
    int sock, msg_sock;
    struct sockaddr_in server_tcp{};
    struct sockaddr_in client_tcp{};
    socklen_t client_tcp_len = sizeof client_tcp;
    socklen_t server_tcp_len = sizeof server_tcp;
    ssize_t write_len;
    ssize_t remaining_file_size = request.param;

    create_tcp_socket(sock, server_tcp, server_tcp_len);
    struct timeval wait_time{options.TIMEOUT, 0};
    fd_set fds;
    bool error_occurred = false;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    char buffer[BSIZE];
    std::string filename = options.SHRD_FLDR + "/" + request.data;

    send_complex_message(state.socket, client_udp, "CAN_ADD", "", request.cmd_seq,
                         ntohs(server_tcp.sin_port));

    /* wait TIMEOUT to connect */
    if (select(sock + 1, &fds, nullptr, nullptr, &wait_time)) {
        client_tcp_len = sizeof(client_tcp);
        msg_sock = accept(sock, (struct sockaddr *) &client_tcp, &client_tcp_len);
        if (msg_sock < 0) {
            throw std::runtime_error("accept");
        }

        int fd;
        ssize_t read_len;
        if ((fd = open(filename.c_str(), O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) {
            throw std::runtime_error("open");
        }
        state.open_files.insert(filename); /* marks the opening of the file */
        read_len = read(msg_sock, buffer, BSIZE);
        while (remaining_file_size > 0 && read_len > 0 && !error_occurred) {
            ssize_t to_write_len = std::min(read_len, remaining_file_size);
            write_len = write(fd, buffer, to_write_len);
            if (write_len != to_write_len) {
                error_occurred = true;
            }
            remaining_file_size -= write_len;
            read_len = read(msg_sock, buffer, BSIZE);
        }
        if (read_len < 0 || remaining_file_size != 0) {
            error_occurred = true;
        }
        if (close(msg_sock) < 0) {
            throw std::runtime_error("close");
        }
    }

    if (error_occurred) {
        unlink(filename.c_str());
    }
    state.open_files.erase(filename); /* marks the closing of the file */
    close(state.socket);
    close(sock);
    exit(0);
}

/** Handle the clients "upload" message. */
void
upload(server_options &options, server_state &state, const struct sockaddr_in &client_address, CMPLX_CMD &request) {
    bool exists = false;
    for (auto &file : state.files) {
        if (file.filename().string() == request.data) {
            exists = true;
            break;
        }
    }

    /* checks available space, if such a file already exists, if the file name
     * contains a '/', if the file name is empty */
    if (state.available_space < request.param || exists ||
        request.data.find('/') != std::string::npos || request.data.empty()) {
        send_simple_message(state.socket, client_address, "NO_WAY", request.data, request.cmd_seq);
    }
    else {
        state.available_space -= request.param;
        fs::path new_file(options.SHRD_FLDR + "/" + request.data);
        state.files.push_back(new_file);
        switch (fork()) {
            case -1:
                throw std::runtime_error("fork");
            case 0:
                receive_file(options, state, client_address, request);
                break;
            default:
                break;
        }
    }
}

/** Server loop. */
void read_requests(server_options &options, server_state &state) {
    /* data received */
    char buffer[BSIZE];
    ssize_t rcv_len;
    struct sockaddr_in client_address{};
    socklen_t addrlen = sizeof client_address;

    for (;;) {
        /* read */
        rcv_len = recvfrom(state.socket, buffer, sizeof buffer, 0, (struct sockaddr *) &client_address, &addrlen);
        if (rcv_len < 0) {
            throw std::runtime_error("read");
        }
        else {
            if (message_too_short<SIMPL_CMD>(client_address, rcv_len)) {
                continue;
            }

            SIMPL_CMD request(buffer, rcv_len);
            if (command_equal(request, "HELLO")) {
                discover(state, options, client_address, request);
            }
            else if (command_equal(request, "DEL")) {
                remove(state, client_address, request);
            }
            else if (command_equal(request, "LIST")) {
                list(state, client_address, request);
            }
            else if (command_equal(request, "GET")) {
                fetch(options, state, client_address, request);
            }
            else if (command_equal(request, "ADD")) {
                if (message_too_short<CMPLX_CMD>(client_address, rcv_len)) {
                    continue;
                }
                CMPLX_CMD complex_request(buffer, rcv_len);
                upload(options, state, client_address, complex_request);
            }
            else {
                error_message(client_address, "Invalid cmd.");
            }
        }
    }
}

int main(int argc, char const *argv[]) {
    try {
        add_signal_handlers();
        server_options options = read_options(argc, argv);
        index_files(options, current_server_state);
        initialize_connection(options, current_server_state);
        read_requests(options, current_server_state);
        clean_up(current_server_state);
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what();
        if (errno != 0) {
            std::cerr << ": " << strerror(errno);
        }
        std::cerr << "\n";
    }
}